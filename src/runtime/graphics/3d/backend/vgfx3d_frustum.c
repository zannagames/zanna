//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_frustum.c
// Purpose: View frustum plane extraction and bounding volume intersection
//   tests. Used by the scene graph (Phase 12) to cull objects outside the
//   camera's view volume before submitting draw calls.
//
// Key invariants:
//   - Gribb-Hartmann method extracts planes from the combined VP matrix.
//   - AABB test uses p-vertex/n-vertex optimization (early-out on first
//     outside plane, tracks intersecting vs fully-inside).
//   - Transform AABB expands the 8 corners and re-fits.
//
// Links: vgfx3d_frustum.h, plans/3d/13-frustum-culling.md
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_frustum.c
 * @brief Implements conservative view-frustum extraction and bounding-volume operations.
 *
 * The implementation derives inward-facing normalized planes from row-major view-projection
 * matrices, classifies axis-aligned boxes and spheres, transforms local AABBs through arbitrary
 * affine matrices, and computes bounds from strided vertex positions. Invalid numeric input
 * deliberately produces conservative results so suspect geometry is not incorrectly culled.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "vgfx3d_frustum.h"
#include "rt_canvas3d_internal.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*==========================================================================
 * Frustum plane extraction (Gribb-Hartmann method)
 *
 * For a row-major VP matrix m[r*4+c], each frustum plane is derived from
 * the sum or difference of two matrix rows. The plane normal points inward
 * (positive half-space = inside the frustum).
 *=========================================================================*/

/// @brief Zero out all six planes, yielding a degenerate frustum that classifies every
///   object as intersecting — the conservative fallback when VP extraction is unusable.
/// @param f Frustum to invalidate; null is a no-op.
static void vgfx3d_frustum_make_conservative(vgfx3d_frustum_t *f) {
    if (!f)
        return;
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 4; j++)
            f->planes[i][j] = 0.0f;
    f->planes_valid = 0;
}

/// @brief True if all three components of a vec3 are finite (no NaN/Inf).
/// @param v Borrowed three-component vector.
/// @return Non-zero when @p v is non-null and every component is finite.
static int vgfx3d_vec3_is_finite(const float v[3]) {
    return v && isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

/// @brief Return the adjacent IEEE-754 float farther along the requested axis.
/// @details `nextafterf` would be the usual library helper for this, but the
///   native-link path audits runtime archives for unresolved dynamic imports.
///   This bit-level step keeps transformed culling bounds conservatively widened
///   by one ULP without adding a libm symbol dependency. Callers pass finite
///   values only; non-finite input is returned unchanged as a defensive fallback.
/// @param value Finite single-precision value to widen.
/// @param toward_positive Non-zero to step toward +infinity, zero toward
///   -infinity.
/// @return The adjacent representable float in the requested direction.
static float vgfx3d_float_step_outward(float value, int toward_positive) {
    typedef union vgfx3d_float_bits_u {
        float f;
        uint32_t u;
    } vgfx3d_float_bits_t;

    if (!isfinite(value))
        return value;
    if (value == 0.0f) {
        vgfx3d_float_bits_t zero_step;
        zero_step.u = toward_positive ? 0x00000001u : 0x80000001u;
        return zero_step.f;
    }

    vgfx3d_float_bits_t bits;
    bits.f = value;
    if ((value > 0.0f) == (toward_positive != 0))
        bits.u += 1u;
    else
        bits.u -= 1u;
    return bits.f;
}

/// @brief Extract the six view-frustum planes from a view-projection matrix.
/// @details Uses Gribb-Hartmann: each plane equation `ax + by + cz + d = 0` is
///   the sum or difference of two rows of a row-major VP matrix. Plane ordering
///   is [Left, Right, Bottom, Top, Near, Far]; normals point inward so a positive
///   signed distance means "inside the frustum". Each plane is normalized so that
///   subsequent distance tests yield true metric distances (needed for the sphere
///   test, which compares against `radius` directly).
/// @param f Caller-owned frustum receiving six planes and their validity state; null is a no-op.
/// @param vp Borrowed 16-element row-major view-projection matrix. Null, non-finite, or degenerate
///           input produces a conservative invalid frustum.
void vgfx3d_frustum_extract(vgfx3d_frustum_t *f, const float vp[16]) {
    if (!f)
        return;
    if (!vp) {
        vgfx3d_frustum_make_conservative(f);
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (!isfinite(vp[i])) {
            vgfx3d_frustum_make_conservative(f);
            return;
        }
    }

    /* Left: row3 + row0 */
    f->planes[0][0] = vp[12] + vp[0];
    f->planes[0][1] = vp[13] + vp[1];
    f->planes[0][2] = vp[14] + vp[2];
    f->planes[0][3] = vp[15] + vp[3];

    /* Right: row3 - row0 */
    f->planes[1][0] = vp[12] - vp[0];
    f->planes[1][1] = vp[13] - vp[1];
    f->planes[1][2] = vp[14] - vp[2];
    f->planes[1][3] = vp[15] - vp[3];

    /* Bottom: row3 + row1 */
    f->planes[2][0] = vp[12] + vp[4];
    f->planes[2][1] = vp[13] + vp[5];
    f->planes[2][2] = vp[14] + vp[6];
    f->planes[2][3] = vp[15] + vp[7];

    /* Top: row3 - row1 */
    f->planes[3][0] = vp[12] - vp[4];
    f->planes[3][1] = vp[13] - vp[5];
    f->planes[3][2] = vp[14] - vp[6];
    f->planes[3][3] = vp[15] - vp[7];

    /* Near: row3 + row2 */
    f->planes[4][0] = vp[12] + vp[8];
    f->planes[4][1] = vp[13] + vp[9];
    f->planes[4][2] = vp[14] + vp[10];
    f->planes[4][3] = vp[15] + vp[11];

    /* Far: row3 - row2 */
    f->planes[5][0] = vp[12] - vp[8];
    f->planes[5][1] = vp[13] - vp[9];
    f->planes[5][2] = vp[14] - vp[10];
    f->planes[5][3] = vp[15] - vp[11];

    /* Normalize each plane so distance tests give metric results */
    for (int i = 0; i < 6; i++) {
        float len = sqrtf(f->planes[i][0] * f->planes[i][0] + f->planes[i][1] * f->planes[i][1] +
                          f->planes[i][2] * f->planes[i][2]);
        if (len > 1e-8f) {
            float inv = 1.0f / len;
            f->planes[i][0] *= inv;
            f->planes[i][1] *= inv;
            f->planes[i][2] *= inv;
            f->planes[i][3] *= inv;
        } else {
            vgfx3d_frustum_make_conservative(f);
            return;
        }
    }
    f->planes_valid = 1;
}

/*==========================================================================
 * AABB frustum test (p-vertex / n-vertex method)
 *
 * For each plane, the p-vertex is the AABB corner MOST in the direction of
 * the plane normal (farthest along the positive side). If the p-vertex is
 * behind the plane, the entire box is outside. The n-vertex is the opposite
 * corner; if it's behind the plane, the box straddles (intersects).
 *=========================================================================*/

/// @brief Classify an axis-aligned box against the frustum.
/// @details Uses the p-vertex / n-vertex optimization: for each plane we select
///   the box corner that sits farthest along the plane normal (p-vertex). If the
///   p-vertex is behind the plane the entire box is outside — that single test
///   rejects without checking the remaining 7 corners. The opposite corner
///   (n-vertex) disambiguates fully-inside from straddling.
/// @param f Borrowed frustum with inward-facing normalized planes.
/// @param min Borrowed minimum AABB corner.
/// @param max Borrowed maximum AABB corner.
/// @return 0 outside, 1 intersecting, 2 fully inside. The 3-valued result lets
///   callers skip recursive culling when the parent volume is fully inside.
int vgfx3d_frustum_test_aabb(const vgfx3d_frustum_t *f, const float min[3], const float max[3]) {
    int result = 2; /* assume fully inside */
    float extent = 0.0f;
    float coord_mag = 0.0f;
    float eps;

    if (!f || !f->planes_valid || !vgfx3d_vec3_is_finite(min) || !vgfx3d_vec3_is_finite(max))
        return 1;
    for (int axis = 0; axis < 3; axis++) {
        if (min[axis] > max[axis])
            return 1;
        if (max[axis] - min[axis] > extent)
            extent = max[axis] - min[axis];
        coord_mag = fmaxf(coord_mag, fmaxf(fabsf(min[axis]), fabsf(max[axis])));
    }
    eps = 1e-4f + fmaxf(fmaxf(extent, coord_mag), 1.0f) * 1e-6f;
    /* Planes were validated once by vgfx3d_frustum_extract; no per-object recheck. */
    for (int i = 0; i < 6; i++) {
        /* Select p-vertex: corner most along plane normal */
        float px = f->planes[i][0] >= 0 ? max[0] : min[0];
        float py = f->planes[i][1] >= 0 ? max[1] : min[1];
        float pz = f->planes[i][2] >= 0 ? max[2] : min[2];
        float dist =
            f->planes[i][0] * px + f->planes[i][1] * py + f->planes[i][2] * pz + f->planes[i][3];
        if (dist < -eps)
            return 0; /* p-vertex behind plane → entirely outside */

        /* Select n-vertex: corner least along plane normal */
        float nx = f->planes[i][0] >= 0 ? min[0] : max[0];
        float ny = f->planes[i][1] >= 0 ? min[1] : max[1];
        float nz = f->planes[i][2] >= 0 ? min[2] : max[2];
        float ndist =
            f->planes[i][0] * nx + f->planes[i][1] * ny + f->planes[i][2] * nz + f->planes[i][3];
        if (ndist < eps)
            result = 1; /* n-vertex behind → intersecting */
    }
    return result;
}

/// @brief Test a padded AABB against the frustum without mutating caller-owned bounds.
/// @details The padding is applied symmetrically in all three axes and then canonicalized so
///   callers can pass local or transformed bounds directly. If the padding cannot be represented
///   safely, the function falls back to the unpadded test instead of manufacturing invalid bounds.
/// @param f Borrowed frustum with inward-facing normalized planes.
/// @param min Borrowed first AABB corner; per-axis ordering is canonicalized when padding applies.
/// @param max Borrowed opposite AABB corner.
/// @param pad Finite positive expansion in world units; non-positive or invalid values request the
///            unpadded classification.
/// @return 0 outside, 1 intersecting, or 2 fully inside.
int vgfx3d_frustum_test_aabb_padded(const vgfx3d_frustum_t *f,
                                    const float min[3],
                                    const float max[3],
                                    float pad) {
    float padded_min[3];
    float padded_max[3];
    if (!isfinite(pad) || pad <= 0.0f)
        return vgfx3d_frustum_test_aabb(f, min, max);
    if (!vgfx3d_vec3_is_finite(min) || !vgfx3d_vec3_is_finite(max))
        return vgfx3d_frustum_test_aabb(f, min, max);
    for (int axis = 0; axis < 3; axis++) {
        float lo = min[axis] <= max[axis] ? min[axis] : max[axis];
        float hi = min[axis] <= max[axis] ? max[axis] : min[axis];
        double expanded_lo = (double)lo - (double)pad;
        double expanded_hi = (double)hi + (double)pad;
        if (!isfinite(expanded_lo) || !isfinite(expanded_hi) || expanded_lo < -(double)FLT_MAX ||
            expanded_hi > (double)FLT_MAX)
            return vgfx3d_frustum_test_aabb(f, min, max);
        padded_min[axis] = (float)expanded_lo;
        padded_max[axis] = (float)expanded_hi;
    }
    return vgfx3d_frustum_test_aabb(f, padded_min, padded_max);
}

/*==========================================================================
 * Bounding sphere frustum test
 *=========================================================================*/

/// @brief Classify a bounding sphere against the frustum.
/// @details Relies on planes having been normalized by `vgfx3d_frustum_extract`
///   so the signed distance from `center` to each plane is measured in world
///   units directly comparable against `radius`. A sphere is outside when any
///   single plane distance is < -radius, intersecting when any distance is
///   within [-radius, +radius], otherwise fully inside.
/// @param f Borrowed frustum with inward-facing normalized planes.
/// @param center Borrowed sphere center in the frustum's coordinate space.
/// @param radius Finite non-negative sphere radius in matching world units.
/// @return 0 outside, 1 intersecting, 2 fully inside.
int vgfx3d_frustum_test_sphere(const vgfx3d_frustum_t *f, const float center[3], float radius) {
    int result = 2; /* assume fully inside */
    if (!f || !f->planes_valid || !vgfx3d_vec3_is_finite(center) || !isfinite(radius) ||
        radius < 0.0f)
        return 1;
    for (int i = 0; i < 6; i++) {
        float dist = f->planes[i][0] * center[0] + f->planes[i][1] * center[1] +
                     f->planes[i][2] * center[2] + f->planes[i][3];
        if (dist < -radius)
            return 0; /* entirely outside */
        if (dist < radius)
            result = 1; /* intersecting */
    }
    return result;
}

/*==========================================================================
 * AABB transform — expand object-space AABB to world-space AABB
 *
 * Transforms all 8 corners of the object-space AABB by the world matrix,
 * then computes the axis-aligned bounding box of the results.
 *=========================================================================*/

/// @brief Re-fit an object-space AABB into world space through a transform.
/// @details Transforms all eight corners of `[obj_min, obj_max]` by `world_matrix`
///   and returns the axis-aligned bounding box of the resulting point set. This
///   is the correct conservative AABB under arbitrary affine (including rotated
///   and non-uniformly scaled) transforms — simply transforming the two diagonal
///   corners would under-fit the result. The 3x3 translation column in row-major
///   convention is `world_matrix[3,7,11]`; the 4th row is ignored because an
///   affine transform leaves w unchanged.
/// @param obj_min Borrowed first object-space AABB corner.
/// @param obj_max Borrowed opposite object-space AABB corner; per-axis ordering is canonicalized.
/// @param world_matrix Borrowed 16-element row-major affine transform using column vectors.
/// @param out_min Receives the conservatively rounded world-space minimum corner.
/// @param out_max Receives the conservatively rounded world-space maximum corner.
/// @return 1 on success, otherwise 0 for null, non-finite, or unrepresentable input. Output values
///         are not meaningful on failure.
int vgfx3d_transform_aabb_checked(const float obj_min[3],
                                  const float obj_max[3],
                                  const double world_matrix[16],
                                  float out_min[3],
                                  float out_max[3]) {
    if (!out_min || !out_max)
        return 0;
    out_min[0] = out_min[1] = out_min[2] = FLT_MAX;
    out_max[0] = out_max[1] = out_max[2] = -FLT_MAX;
    if (!obj_min || !obj_max || !world_matrix) {
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        if (!isfinite(obj_min[i]) || !isfinite(obj_max[i])) {
            return 0;
        }
    }
    for (int i = 0; i < 16; i++) {
        if (!isfinite(world_matrix[i])) {
            return 0;
        }
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

    /* Iterate all 8 corners of the AABB */
    for (int i = 0; i < 8; i++) {
        float cx = (i & 1) ? safe_max[0] : safe_min[0];
        float cy = (i & 2) ? safe_max[1] : safe_min[1];
        float cz = (i & 4) ? safe_max[2] : safe_min[2];

        /* Transform by row-major 4x4 matrix (column-vector convention) */
        double dx =
            world_matrix[0] * cx + world_matrix[1] * cy + world_matrix[2] * cz + world_matrix[3];
        double dy =
            world_matrix[4] * cx + world_matrix[5] * cy + world_matrix[6] * cz + world_matrix[7];
        double dz =
            world_matrix[8] * cx + world_matrix[9] * cy + world_matrix[10] * cz + world_matrix[11];
        if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz) || dx < -FLT_MAX || dx > FLT_MAX ||
            dy < -FLT_MAX || dy > FLT_MAX || dz < -FLT_MAX || dz > FLT_MAX) {
            return 0;
        }
        float wx = (float)dx;
        float wy = (float)dy;
        float wz = (float)dz;

        if (wx < out_min[0])
            out_min[0] = wx;
        if (wy < out_min[1])
            out_min[1] = wy;
        if (wz < out_min[2])
            out_min[2] = wz;
        if (wx > out_max[0])
            out_max[0] = wx;
        if (wy > out_max[1])
            out_max[1] = wy;
        if (wz > out_max[2])
            out_max[2] = wz;
    }
    for (int axis = 0; axis < 3; axis++) {
        if (out_min[axis] > out_max[axis]) {
            float tmp = out_min[axis];
            out_min[axis] = out_max[axis];
            out_max[axis] = tmp;
        }
        if (out_min[axis] > -FLT_MAX)
            out_min[axis] = vgfx3d_float_step_outward(out_min[axis], 0);
        if (out_max[axis] < FLT_MAX)
            out_max[axis] = vgfx3d_float_step_outward(out_max[axis], 1);
    }
    return 1;
}

/// @brief Legacy AABB transform wrapper that preserves the historical zero-on-error behavior.
/// @param obj_min Borrowed first object-space AABB corner.
/// @param obj_max Borrowed opposite object-space AABB corner.
/// @param world_matrix Borrowed 16-element row-major affine transform.
/// @param out_min Receives the transformed minimum, or zero on failure.
/// @param out_max Receives the transformed maximum, or zero on failure.
void vgfx3d_transform_aabb(const float obj_min[3],
                           const float obj_max[3],
                           const double world_matrix[16],
                           float out_min[3],
                           float out_max[3]) {
    if (vgfx3d_transform_aabb_checked(obj_min, obj_max, world_matrix, out_min, out_max))
        return;
    if (!out_min || !out_max)
        return;
    out_min[0] = out_min[1] = out_min[2] = 0.0f;
    out_max[0] = out_max[1] = out_max[2] = 0.0f;
}

/*==========================================================================
 * Compute mesh AABB from vertex positions
 *=========================================================================*/

/// @brief Compute the axis-aligned bounding box of a strided vertex array.
/// @details Treats `vertices` as a byte buffer and reads three floats from each
///   stride-sized slot (matching the in-memory layout of `vgfx3d_vertex_t`,
///   where the position occupies the first three floats). Empty or null input
///   yields a zero-sized AABB at the origin rather than inverted sentinels, so
///   downstream code doesn't need a special "empty mesh" branch. Non-finite
///   vertices are ignored when at least one finite vertex is present; an array
///   with no finite positions returns a deliberately huge conservative box so
///   culling and occlusion cannot accidentally hide suspect geometry.
/// @param vertices Borrowed byte-addressable vertex array whose first three floats are position.
/// @param vertex_count Number of strided vertex records.
/// @param vertex_stride Byte distance between consecutive vertex position slots.
/// @param out_min Receives the minimum finite position on each axis.
/// @param out_max Receives the maximum finite position on each axis.
void vgfx3d_compute_mesh_aabb(const void *vertices,
                              uint32_t vertex_count,
                              uint32_t vertex_stride,
                              float out_min[3],
                              float out_max[3]) {
    if (!out_min || !out_max)
        return;
    out_min[0] = out_min[1] = out_min[2] = FLT_MAX;
    out_max[0] = out_max[1] = out_max[2] = -FLT_MAX;

    if (!vertices || vertex_count == 0 || vertex_stride < sizeof(float) * 3u ||
        ((size_t)vertex_count > 1u && vertex_stride > SIZE_MAX / ((size_t)vertex_count - 1u))) {
        out_min[0] = out_min[1] = out_min[2] = 0.0f;
        out_max[0] = out_max[1] = out_max[2] = 0.0f;
        return;
    }

    const uint8_t *base = (const uint8_t *)vertices;
    int found = 0;
    for (uint32_t i = 0; i < vertex_count; i++) {
        float pos[3];
        memcpy(pos, base + (size_t)i * (size_t)vertex_stride, sizeof(pos));
        if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2]))
            continue;
        for (int j = 0; j < 3; j++) {
            if (pos[j] < out_min[j])
                out_min[j] = pos[j];
            if (pos[j] > out_max[j])
                out_max[j] = pos[j];
        }
        found = 1;
    }
    if (!found) {
        float conservative = FLT_MAX * 0.25f;
        out_min[0] = out_min[1] = out_min[2] = -conservative;
        out_max[0] = out_max[1] = out_max[2] = conservative;
    }
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
