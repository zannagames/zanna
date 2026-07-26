//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_raycast3d.c
// Purpose: 3D raycasting and AABB collision detection for picking, shooting,
//   and physics. Implements Möller–Trumbore ray-triangle intersection, slab
//   method ray-AABB, quadratic ray-sphere, and AABB overlap/penetration.
//
// Key invariants:
//   - Public ray queries normalize non-zero directions internally, so distances are world units.
//   - Möller–Trumbore returns parametric t; t < 0 means behind ray origin.
//   - Ray-mesh transforms the ray into object space via inverse model matrix and
//     traverses a retained geometry-revision-keyed BVH.
//   - Singular transforms retain the exact world-space linear triangle fallback.
//   - AABB penetration returns the minimum push-out vector (shortest axis).
//   - Capsule-vs-AABB uses exact segment-to-AABB distance.
//
// Ownership/Lifetime:
//   - RayHit3D is GC-managed; no finalizer needed (no owned heap allocations).
//   - Returned Vec3 hit-point / normal handles are caller-owned.
//   - Mesh3D owns retained raycast BVH nodes and triangle permutations.
//
// Links: rt_raycast3d.h, rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements finite, allocation-aware 3D ray and primitive intersection queries.
/// @details Public queries validate boxed Vec3/Mat4/Mesh handles, normalize ray
/// directions so reported distances use world units, and sanitize untrusted
/// floating-point inputs. Mesh queries use a revision-keyed retained BVH with
/// exact linear fallback for singular transforms or allocation failure.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_raycast3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_mat4.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Allocate a runtime object payload with a stable class identifier.
/// @param class_id Runtime class identifier stored in the heap header.
/// @param byte_size Payload size in bytes.
/// @return New object payload, or `NULL` on allocation failure.
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);

/// @brief Install a finalizer on a runtime object.
/// @param obj Borrowed object payload.
/// @param fn Finalizer callback, or `NULL` to clear it.
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));

/// @brief Allocate a boxed Vec3 with the supplied components.
/// @param x X component.
/// @param y Y component.
/// @param z Z component.
/// @return New Vec3 handle.
extern void *rt_vec3_new(double x, double y, double z);

/// @brief Read the X component of a boxed Vec3.
/// @param v Borrowed Vec3 handle.
/// @return Stored X component.
extern double rt_vec3_x(void *v);

/// @brief Read the Y component of a boxed Vec3.
/// @param v Borrowed Vec3 handle.
/// @return Stored Y component.
extern double rt_vec3_y(void *v);

/// @brief Read the Z component of a boxed Vec3.
/// @param v Borrowed Vec3 handle.
/// @return Stored Z component.
extern double rt_vec3_z(void *v);

#define EPSILON 1e-8
#define RAYCAST3D_COORD_ABS_MAX 1000000000000.0
#define RAYCAST3D_DISTANCE_MAX 1000000000.0

/// @brief Clamp a scalar into the closed range `[lo, hi]`. Used by the per-axis closest-
/// point query and the box-projection paths that map an arbitrary point onto an AABB.
/// @param v Candidate scalar.
/// @param lo Lower bound; non-finite values become zero.
/// @param hi Upper bound; non-finite values collapse to the lower bound.
/// @return Finite scalar in the canonicalized closed interval.
static double clampd(double v, double lo, double hi) {
    if (!isfinite(lo))
        lo = 0.0;
    if (!isfinite(hi))
        hi = lo;
    if (lo > hi) {
        double tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (!isfinite(v))
        return lo;
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/// @brief Return non-zero if all three components of the raw double[3] vector are finite (not
/// NaN/inf).
/// @param v Borrowed three-component vector array.
/// @return Nonzero when the pointer and all components are finite.
static int vec3_is_finite_raw(const double *v) {
    return v && isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

/// @brief Clamp scene-ray coordinates to a broad finite runtime range.
/// @param value Candidate coordinate.
/// @return Finite coordinate in `[-RAYCAST3D_COORD_ABS_MAX, RAYCAST3D_COORD_ABS_MAX]`.
static double raycast3d_saturate_coord(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value > RAYCAST3D_COORD_ABS_MAX)
        return RAYCAST3D_COORD_ABS_MAX;
    if (value < -RAYCAST3D_COORD_ABS_MAX)
        return -RAYCAST3D_COORD_ABS_MAX;
    return value;
}

/// @brief Clamp non-negative ray distances/radii to a finite runtime range.
/// @param value Candidate distance or radius.
/// @return Positive finite value capped at the runtime maximum, or zero when invalid.
static double raycast3d_sanitize_distance(double value) {
    if (!isfinite(value) || value <= 0.0)
        return 0.0;
    return value > RAYCAST3D_DISTANCE_MAX ? RAYCAST3D_DISTANCE_MAX : value;
}

/// @brief Sanitize a non-negative hit distance while preserving -1 as the miss sentinel.
/// @param value Candidate hit distance.
/// @return Finite capped distance, or `-1` for a miss or invalid value.
static double raycast3d_sanitize_hit_distance(double value) {
    if (!isfinite(value) || value < 0.0)
        return -1.0;
    return value > RAYCAST3D_DISTANCE_MAX ? RAYCAST3D_DISTANCE_MAX : value;
}

/// @brief Sanitize an in-place raw vector.
/// @param v Mutable three-component vector; `NULL` is accepted.
static void vec3_sanitize_raw(double *v) {
    if (!v)
        return;
    v[0] = raycast3d_saturate_coord(v[0]);
    v[1] = raycast3d_saturate_coord(v[1]);
    v[2] = raycast3d_saturate_coord(v[2]);
}

/// @brief Read a boxed Vec3 handle into `out[3]`; returns 0 (and leaves caller to
///        reject) when `obj` is not a Vec3 or any component is non-finite.
/// @param obj Borrowed opaque handle expected to contain a Vec3.
/// @param out Output array receiving sanitized components.
/// @return Nonzero when a finite Vec3 was read successfully.
static int vec3_read_finite(void *obj, double *out) {
    if (!out || !rt_g3d_is_vec3(obj))
        return 0;
    out[0] = rt_vec3_x(obj);
    out[1] = rt_vec3_y(obj);
    out[2] = rt_vec3_z(obj);
    if (!vec3_is_finite_raw(out))
        return 0;
    vec3_sanitize_raw(out);
    return 1;
}

/// @brief Normalize a vec3 in place; returns 0 (leaving @p v unchanged) if it is
///   non-finite or shorter than EPSILON, else 1.
/// @param v Mutable three-component vector.
/// @return Nonzero after successful normalization, otherwise zero.
static int vec3_normalize_raw(double *v) {
    double len_sq;
    double inv_len;
    if (!vec3_is_finite_raw(v))
        return 0;
    len_sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (!isfinite(len_sq) || len_sq < EPSILON * EPSILON)
        return 0;
    inv_len = 1.0 / sqrt(len_sq);
    v[0] *= inv_len;
    v[1] *= inv_len;
    v[2] *= inv_len;
    return 1;
}

/// @brief Normalize a raw normal, falling back to +Y when invalid or degenerate.
/// @param v Mutable three-component normal.
static void vec3_normalize_or_up_raw(double *v) {
    if (!vec3_normalize_raw(v)) {
        v[0] = 0.0;
        v[1] = 1.0;
        v[2] = 0.0;
    }
}

/// @brief Allocate a Vec3 after applying the scene-raycast coordinate clamp.
/// @param x Candidate X component.
/// @param y Candidate Y component.
/// @param z Candidate Z component.
/// @return New sanitized Vec3 handle.
static void *vec3_new_sanitized(double x, double y, double z) {
    return rt_vec3_new(
        raycast3d_saturate_coord(x), raycast3d_saturate_coord(y), raycast3d_saturate_coord(z));
}

/// @brief Checked cast of an opaque handle to a Mat4 payload; NULL on class mismatch.
/// @param obj Borrowed opaque handle.
/// @return Borrowed Mat4 payload on a valid class/heap match, otherwise `NULL`.
static mat4_impl *raycast3d_mat4_checked(void *obj) {
    if (!obj)
        return NULL;
    if (!rt_heap_is_payload(obj) || rt_obj_class_id(obj) != RT_MAT4_CLASS_ID)
        return NULL;
    return (mat4_impl *)obj;
}

/// @brief True if all 16 lanes of a 4x4 double matrix are finite (no NaN/Inf).
/// @param m Borrowed row-major 16-element matrix array.
/// @return Nonzero when the pointer and all lanes are finite.
static int mat4d_is_finite(const double *m) {
    if (!m)
        return 0;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(m[i]))
            return 0;
    }
    return 1;
}

/// @brief Canonicalize an AABB in place by swapping any axis where min > max,
///        so subsequent overlap/clamp tests can assume `mn[i] <= mx[i]`.
/// @param mn Mutable three-component minimum corner.
/// @param mx Mutable three-component maximum corner.
static void aabb3d_canonicalize_raw(double *mn, double *mx) {
    if (!mn || !mx)
        return;
    vec3_sanitize_raw(mn);
    vec3_sanitize_raw(mx);
    for (int i = 0; i < 3; i++) {
        if (mn[i] > mx[i]) {
            double tmp = mn[i];
            mn[i] = mx[i];
            mx[i] = tmp;
        }
    }
}

/// @brief Find the closest point on segment [a, b] to @p point (raw double[3] form).
/// @details Projects @p point onto the line through a and b, clamps the projection
///   parameter to [0, 1] to stay within the segment, and writes the result to @p closest.
///   Degenerate (zero-length) segments collapse to point a. Used by capsule overlap tests.
/// @param a Borrowed first segment endpoint.
/// @param b Borrowed second segment endpoint.
/// @param point Borrowed query point.
/// @param closest Output array receiving the sanitized closest point.
static void segment3d_closest_point_raw(const double *a,
                                        const double *b,
                                        const double *point,
                                        double *closest) {
    double aa[3] = {a ? a[0] : 0.0, a ? a[1] : 0.0, a ? a[2] : 0.0};
    double bb[3] = {b ? b[0] : 0.0, b ? b[1] : 0.0, b ? b[2] : 0.0};
    double pp[3] = {point ? point[0] : 0.0, point ? point[1] : 0.0, point ? point[2] : 0.0};
    if (!closest)
        return;
    vec3_sanitize_raw(aa);
    vec3_sanitize_raw(bb);
    vec3_sanitize_raw(pp);
    double dx = bb[0] - aa[0];
    double dy = bb[1] - aa[1];
    double dz = bb[2] - aa[2];
    double len_sq = dx * dx + dy * dy + dz * dz;
    if (!isfinite(len_sq) || len_sq < 1e-12) {
        closest[0] = aa[0];
        closest[1] = aa[1];
        closest[2] = aa[2];
        return;
    }
    double t = ((pp[0] - aa[0]) * dx + (pp[1] - aa[1]) * dy + (pp[2] - aa[2]) * dz) / len_sq;
    t = clampd(t, 0.0, 1.0);
    closest[0] = aa[0] + t * dx;
    closest[1] = aa[1] + t * dy;
    closest[2] = aa[2] + t * dz;
    vec3_sanitize_raw(closest);
}

/// @brief Project a 3-point onto an axis-aligned bounding box by clamping each
/// coordinate independently into the box's extent. The result is the closest point on
/// (or inside) the box to the input point. Used by sphere-vs-AABB and capsule-vs-AABB
/// overlap tests; takes raw double arrays so the inner-loop tests don't have to allocate
/// throwaway Vec3 wrappers.
/// @param mn Borrowed AABB minimum corner.
/// @param mx Borrowed AABB maximum corner.
/// @param point Borrowed query point.
/// @param closest Output array receiving the sanitized closest point.
static void aabb3d_clamp_point_raw(const double *mn,
                                   const double *mx,
                                   const double *point,
                                   double *closest) {
    double local_min[3] = {mn ? mn[0] : 0.0, mn ? mn[1] : 0.0, mn ? mn[2] : 0.0};
    double local_max[3] = {mx ? mx[0] : 0.0, mx ? mx[1] : 0.0, mx ? mx[2] : 0.0};
    double p[3] = {point ? point[0] : 0.0, point ? point[1] : 0.0, point ? point[2] : 0.0};
    if (!closest)
        return;
    aabb3d_canonicalize_raw(local_min, local_max);
    vec3_sanitize_raw(p);
    closest[0] = clampd(p[0], local_min[0], local_max[0]);
    closest[1] = clampd(p[1], local_min[1], local_max[1]);
    closest[2] = clampd(p[2], local_min[2], local_max[2]);
    vec3_sanitize_raw(closest);
}

/// @brief Squared distance from point `p` to AABB [mn,mx] (0 when `p` is inside).
/// @param mn Borrowed AABB minimum corner.
/// @param mx Borrowed AABB maximum corner.
/// @param p Borrowed query point.
/// @return Finite squared distance, or `DBL_MAX` when arithmetic is invalid.
static double point_aabb_distance_sq_raw(const double *mn, const double *mx, const double *p) {
    double c[3];
    aabb3d_clamp_point_raw(mn, mx, p, c);
    double pp[3] = {p ? p[0] : 0.0, p ? p[1] : 0.0, p ? p[2] : 0.0};
    vec3_sanitize_raw(pp);
    double dx = pp[0] - c[0];
    double dy = pp[1] - c[1];
    double dz = pp[2] - c[2];
    double dist_sq = dx * dx + dy * dy + dz * dz;
    return isfinite(dist_sq) ? dist_sq : DBL_MAX;
}

/// @brief Evaluate the parametric point `a + d*t` into `out[3]`.
/// @param a Borrowed segment origin.
/// @param d Borrowed segment displacement.
/// @param t Segment parameter clamped to `[0, 1]`.
/// @param out Output array receiving the sanitized point.
static void segment_point_at_raw(const double *a, const double *d, double t, double *out) {
    double aa[3] = {a ? a[0] : 0.0, a ? a[1] : 0.0, a ? a[2] : 0.0};
    double dd[3] = {d ? d[0] : 0.0, d ? d[1] : 0.0, d ? d[2] : 0.0};
    if (!out)
        return;
    vec3_sanitize_raw(aa);
    vec3_sanitize_raw(dd);
    t = clampd(t, 0.0, 1.0);
    out[0] = aa[0] + dd[0] * t;
    out[1] = aa[1] + dd[1] * t;
    out[2] = aa[2] + dd[2] * t;
    vec3_sanitize_raw(out);
}

/// @brief Squared distance between segment a–b and AABB [mn,mx].
/// @details Builds a candidate set of segment parameters — the endpoints plus
///          each axis slab-boundary crossing in `(0,1)` — sorts them, then takes
///          the minimum point-to-AABB distance over those samples and the
///          midpoints between consecutive samples. A sampling approximation that
///          is exact at the breakpoints where the nearest box feature changes.
/// @param a Borrowed first segment endpoint.
/// @param b Borrowed second segment endpoint.
/// @param mn Borrowed AABB minimum corner.
/// @param mx Borrowed AABB maximum corner.
/// @return Finite minimum squared distance, or `DBL_MAX` when no valid value is produced.
static double segment_aabb_distance_sq_raw(const double *a,
                                           const double *b,
                                           const double *mn,
                                           const double *mx) {
    double aa[3] = {a ? a[0] : 0.0, a ? a[1] : 0.0, a ? a[2] : 0.0};
    double bb[3] = {b ? b[0] : 0.0, b ? b[1] : 0.0, b ? b[2] : 0.0};
    double local_min[3] = {mn ? mn[0] : 0.0, mn ? mn[1] : 0.0, mn ? mn[2] : 0.0};
    double local_max[3] = {mx ? mx[0] : 0.0, mx ? mx[1] : 0.0, mx ? mx[2] : 0.0};
    vec3_sanitize_raw(aa);
    vec3_sanitize_raw(bb);
    aabb3d_canonicalize_raw(local_min, local_max);
    double d[3] = {bb[0] - aa[0], bb[1] - aa[1], bb[2] - aa[2]};
    double ts[8];
    int count = 0;
    double best = DBL_MAX;
    ts[count++] = 0.0;
    ts[count++] = 1.0;
    for (int axis = 0; axis < 3; axis++) {
        if (fabs(d[axis]) <= EPSILON)
            continue;
        double t0 = (local_min[axis] - aa[axis]) / d[axis];
        double t1 = (local_max[axis] - aa[axis]) / d[axis];
        if (!isfinite(t0) || !isfinite(t1))
            continue;
        if (t0 > 0.0 && t0 < 1.0)
            ts[count++] = t0;
        if (t1 > 0.0 && t1 < 1.0)
            ts[count++] = t1;
    }
    for (int i = 1; i < count; i++) {
        double key = ts[i];
        int j = i - 1;
        while (j >= 0 && ts[j] > key) {
            ts[j + 1] = ts[j];
            j--;
        }
        ts[j + 1] = key;
    }
    for (int i = 0; i < count; i++) {
        double p[3];
        segment_point_at_raw(aa, d, ts[i], p);
        double dist_sq = point_aabb_distance_sq_raw(local_min, local_max, p);
        if (isfinite(dist_sq) && dist_sq < best)
            best = dist_sq;
    }
    for (int i = 0; i + 1 < count; i++) {
        double lo = ts[i];
        double hi = ts[i + 1];
        double mid = (lo + hi) * 0.5;
        double denom = 0.0;
        double numer = 0.0;
        for (int axis = 0; axis < 3; axis++) {
            double pm = aa[axis] + d[axis] * mid;
            double boundary;
            if (pm < local_min[axis])
                boundary = local_min[axis];
            else if (pm > local_max[axis])
                boundary = local_max[axis];
            else
                continue;
            denom += d[axis] * d[axis];
            numer += d[axis] * (aa[axis] - boundary);
        }
        if (isfinite(denom) && isfinite(numer) && denom > EPSILON) {
            double t = -numer / denom;
            if (isfinite(t) && t > lo && t < hi) {
                double p[3];
                segment_point_at_raw(aa, d, t, p);
                double dist_sq = point_aabb_distance_sq_raw(local_min, local_max, p);
                if (isfinite(dist_sq) && dist_sq < best)
                    best = dist_sq;
            }
        }
    }
    return isfinite(best) ? best : DBL_MAX;
}

/// @brief Slab-method ray-vs-AABB intersection. For each axis, intersect the ray with
/// the two parallel slab planes and accumulate `[tmin, tmax]` for the union of axes.
/// Special-cases parallel rays (|dir| < EPSILON) by checking origin containment in the
/// slab. Returns the smallest non-negative `t` (entry distance) or 0 when the ray
/// originates inside the box; -1 when no intersection. The all-double signature lets
/// hot inner loops skip Vec3 boxing.
/// @param origin Borrowed raw ray origin.
/// @param dir Borrowed non-degenerate raw ray direction.
/// @param mn Borrowed AABB minimum corner.
/// @param mx Borrowed AABB maximum corner.
/// @return Non-negative parametric entry distance, or `-1` on a miss.
static double rt_ray3d_intersect_aabb_raw(const double *origin,
                                          const double *dir,
                                          const double *mn,
                                          const double *mx) {
    double o[3] = {origin ? origin[0] : 0.0, origin ? origin[1] : 0.0, origin ? origin[2] : 0.0};
    double d[3] = {dir ? dir[0] : 0.0, dir ? dir[1] : 0.0, dir ? dir[2] : 0.0};
    double local_min[3] = {mn ? mn[0] : 0.0, mn ? mn[1] : 0.0, mn ? mn[2] : 0.0};
    double local_max[3] = {mx ? mx[0] : 0.0, mx ? mx[1] : 0.0, mx ? mx[2] : 0.0};
    double tmin = -DBL_MAX, tmax = DBL_MAX;
    vec3_sanitize_raw(o);
    vec3_sanitize_raw(d);
    aabb3d_canonicalize_raw(local_min, local_max);
    double len_sq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (!isfinite(len_sq) || len_sq < EPSILON * EPSILON)
        return -1.0;
    for (int i = 0; i < 3; i++) {
        if (fabs(d[i]) < EPSILON) {
            if (o[i] < local_min[i] || o[i] > local_max[i])
                return -1.0;
            continue;
        }
        {
            double inv = 1.0 / d[i];
            double t0 = (local_min[i] - o[i]) * inv;
            double t1 = (local_max[i] - o[i]) * inv;
            if (!isfinite(t0) || !isfinite(t1))
                return -1.0;
            if (t0 > t1) {
                double tmp = t0;
                t0 = t1;
                t1 = tmp;
            }
            if (t0 > tmin)
                tmin = t0;
            if (t1 < tmax)
                tmax = t1;
            if (tmin > tmax)
                return -1.0;
        }
    }
    {
        double t = tmin >= 0.0 ? tmin : (tmax >= 0.0 ? 0.0 : -1.0);
        return isfinite(t) ? t : -1.0;
    }
}

/// @brief Transform a 3-point through a 4×4 matrix as `M * (point, 1)`. Drops the w
/// row (assumed identity for the affine transforms used here). Allocation-free wrapper
/// used by ray-into-mesh-local-space conversion before triangle intersection tests.
/// @param m Borrowed finite row-major 4-by-4 matrix.
/// @param point Borrowed three-component point.
/// @param out Output array receiving the transformed sanitized point.
static void mat4_transform_point_raw(const double *m, const double *point, double *out) {
    double p[3] = {point ? point[0] : 0.0, point ? point[1] : 0.0, point ? point[2] : 0.0};
    double x;
    double y;
    double z;
    if (!out)
        return;
    vec3_sanitize_raw(p);
    if (!mat4d_is_finite(m)) {
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        return;
    }
    x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
    out[0] = raycast3d_saturate_coord(x);
    out[1] = raycast3d_saturate_coord(y);
    out[2] = raycast3d_saturate_coord(z);
}

/// @brief Invert a 4×4 row-major matrix using the cofactor / adjugate method. Computes
/// each adjugate entry as the signed minor of the transpose, then divides by the
/// determinant (taken from the first row · first row of cofactors). Returns 0 on
/// success, -1 when |det| < 1e-12 (singular). Used by raycast queries to bring a
/// world-space ray into a mesh's local space for triangle intersection.
/// @param m Borrowed finite row-major 4-by-4 matrix.
/// @param out Output array receiving the inverse matrix.
/// @return Zero on success, or `-1` for invalid, singular, or non-finite input.
static int mat4d_invert(const double *m, double *out) {
    double inv[16];
    if (!out || !mat4d_is_finite(m))
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

    {
        for (int i = 0; i < 16; i++) {
            if (!isfinite(inv[i]))
                return -1;
        }
        double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (!isfinite(det) || fabs(det) < 1e-12)
            return -1;
        det = 1.0 / det;
        for (int i = 0; i < 16; i++) {
            out[i] = inv[i] * det;
            if (!isfinite(out[i]))
                return -1;
        }
    }
    return 0;
}

/*==========================================================================
 * RayHit3D — result of ray-mesh intersection
 *=========================================================================*/

/// @brief GC-managed world-space result of a mesh raycast.
typedef struct {
    /// Sanitized Euclidean distance from the world-space ray origin.
    double distance;
    /// Sanitized world-space hit position.
    double point[3];
    /// Unit world-space geometric face normal.
    double normal[3];
    /// Deterministic source triangle index within the mesh.
    int64_t triangle_index;
} rt_rayhit3d;

/*==========================================================================
 * Möller–Trumbore ray-triangle intersection
 *
 * Returns parametric distance t (>= 0 = hit), or -1 if miss.
 * Edge vectors e1 = v1-v0, e2 = v2-v0. Uses cross products to compute
 * barycentric coordinates (u, v) and distance t simultaneously.
 *=========================================================================*/

/// @brief Möller–Trumbore ray-triangle intersection. Returns the Euclidean distance
/// `t` along the ray to the hit point (≥ 0 on hit), or -1 on miss / NULL inputs /
/// degenerate (parallel) ray. Computes barycentric coordinates inline and rejects
/// hits with `u`, `v`, or `1 - u - v` outside `[0, 1]`. The classic algorithm — no
/// precomputation required, branch-light enough for tight inner loops over triangle
/// soups.
/// @param origin Borrowed boxed Vec3 ray origin.
/// @param dir Borrowed boxed Vec3 direction, normalized internally.
/// @param v0_obj Borrowed boxed Vec3 first triangle vertex.
/// @param v1_obj Borrowed boxed Vec3 second triangle vertex.
/// @param v2_obj Borrowed boxed Vec3 third triangle vertex.
/// @param front_only Nonzero to reject triangles facing away from the ray.
/// @return Euclidean hit distance, or `-1` for invalid input, a miss, or a culled face.
static double ray3d_intersect_triangle_impl(
    void *origin, void *dir, void *v0_obj, void *v1_obj, void *v2_obj, int front_only) {
    double o[3];
    double d[3];
    double a_pt[3];
    double b_pt[3];
    double c_pt[3];
    if (!vec3_read_finite(origin, o) || !vec3_read_finite(dir, d) ||
        !vec3_read_finite(v0_obj, a_pt) || !vec3_read_finite(v1_obj, b_pt) ||
        !vec3_read_finite(v2_obj, c_pt))
        return -1.0;
    if (!vec3_normalize_raw(d))
        return -1.0;

    double ox = o[0], oy = o[1], oz = o[2];
    double dx = d[0], dy = d[1], dz = d[2];

    double ax = a_pt[0], ay = a_pt[1], az = a_pt[2];
    double bx = b_pt[0], by = b_pt[1], bz = b_pt[2];
    double cx = c_pt[0], cy = c_pt[1], cz = c_pt[2];

    /* Edge vectors */
    double e1x = bx - ax, e1y = by - ay, e1z = bz - az;
    double e2x = cx - ax, e2y = cy - ay, e2z = cz - az;

    /* P = dir × e2 */
    double px = dy * e2z - dz * e2y;
    double py = dz * e2x - dx * e2z;
    double pz = dx * e2y - dy * e2x;
    if (!isfinite(px) || !isfinite(py) || !isfinite(pz))
        return -1.0;

    double det = e1x * px + e1y * py + e1z * pz;
    /* Front-only mode rejects negative determinants (back-facing, CCW winding) so
     * picking and line-of-sight queries can ignore a mesh's interior faces. */
    if (!isfinite(det) || (front_only ? det < EPSILON : fabs(det) < EPSILON))
        return -1.0; /* parallel (or culled backface) */

    double inv_det = 1.0 / det;

    /* T = origin - v0 */
    double tx = ox - ax, ty = oy - ay, tz = oz - az;

    /* u = T · P * inv_det */
    double u = (tx * px + ty * py + tz * pz) * inv_det;
    if (!isfinite(u) || u < 0.0 || u > 1.0)
        return -1.0;

    /* Q = T × e1 */
    double qx = ty * e1z - tz * e1y;
    double qy = tz * e1x - tx * e1z;
    double qz = tx * e1y - ty * e1x;

    /* v = dir · Q * inv_det */
    double v = (dx * qx + dy * qy + dz * qz) * inv_det;
    if (!isfinite(v) || v < 0.0 || u + v > 1.0)
        return -1.0;

    /* t = e2 · Q * inv_det */
    double t = (e2x * qx + e2y * qy + e2z * qz) * inv_det;
    return isfinite(t) && t >= 0.0 ? raycast3d_sanitize_hit_distance(t) : -1.0;
}

/// @brief Intersect a two-sided triangle with a ray using Möller–Trumbore.
/// @param origin Borrowed boxed Vec3 ray origin.
/// @param dir Borrowed boxed Vec3 direction, normalized internally.
/// @param v0_obj Borrowed boxed Vec3 first triangle vertex.
/// @param v1_obj Borrowed boxed Vec3 second triangle vertex.
/// @param v2_obj Borrowed boxed Vec3 third triangle vertex.
/// @return Euclidean hit distance, or `-1` for invalid input or a miss.
double rt_ray3d_intersect_triangle(
    void *origin, void *dir, void *v0_obj, void *v1_obj, void *v2_obj) {
    return ray3d_intersect_triangle_impl(origin, dir, v0_obj, v1_obj, v2_obj, 0);
}

/// @brief Ray/triangle intersection with optional backface culling.
/// @details With @p front_only non-zero, triangles whose winding faces away from the ray are
///          rejected — the natural mode for picking and line-of-sight queries where hits on a
///          mesh's interior faces are surprising. Zero preserves the two-sided behavior.
/// @param origin Borrowed boxed Vec3 ray origin.
/// @param dir Borrowed boxed Vec3 direction, normalized internally.
/// @param v0_obj Borrowed boxed Vec3 first triangle vertex.
/// @param v1_obj Borrowed boxed Vec3 second triangle vertex.
/// @param v2_obj Borrowed boxed Vec3 third triangle vertex.
/// @param front_only Nonzero to reject back-facing triangles.
/// @return Euclidean hit distance, or `-1` for invalid input, a miss, or a culled face.
double rt_ray3d_intersect_triangle_cull(
    void *origin, void *dir, void *v0_obj, void *v1_obj, void *v2_obj, int8_t front_only) {
    return ray3d_intersect_triangle_impl(origin, dir, v0_obj, v1_obj, v2_obj, front_only ? 1 : 0);
}

/*==========================================================================
 * Ray-AABB intersection (slab method)
 *=========================================================================*/

/// @brief Test ray–AABB intersection using the slab method.
/// @details Returns the nearest positive intersection distance, or -1.0 on miss.
///          Uses the standard slab algorithm: project the ray onto each axis,
///          compute entry/exit intervals, and check for overlap.
/// @param origin   Vec3 ray origin.
/// @param dir      Vec3 ray direction (need not be normalized; zero-length misses).
/// @param aabb_min Vec3 minimum corner of the axis-aligned bounding box.
/// @param aabb_max Vec3 maximum corner of the axis-aligned bounding box.
/// @return Euclidean distance to the nearest hit, or -1.0 on miss.
double rt_ray3d_intersect_aabb(void *origin, void *dir, void *aabb_min, void *aabb_max) {
    double o[3], d[3], mn[3], mx[3];
    double t;
    if (!vec3_read_finite(origin, o) || !vec3_read_finite(dir, d) ||
        !vec3_read_finite(aabb_min, mn) || !vec3_read_finite(aabb_max, mx))
        return -1.0;
    if (!vec3_normalize_raw(d))
        return -1.0;
    aabb3d_canonicalize_raw(mn, mx);
    t = rt_ray3d_intersect_aabb_raw(o, d, mn, mx);
    return t >= 0.0 ? raycast3d_sanitize_hit_distance(t) : -1.0;
}

/*==========================================================================
 * Ray-sphere intersection (quadratic formula)
 *=========================================================================*/

/// @brief Test ray–sphere intersection using the quadratic formula.
/// @details Solves the quadratic |O + tD - C|² = r² for the smallest positive t.
///          Returns -1.0 on miss. The ray origin may be inside the sphere (returns 0 distance).
/// @param origin Vec3 ray origin.
/// @param dir    Vec3 ray direction.
/// @param center Vec3 sphere center.
/// @param radius Sphere radius.
/// @return Distance t to nearest hit, or -1.0 on miss.
double rt_ray3d_intersect_sphere(void *origin, void *dir, void *center, double radius) {
    double o[3], d[3], cpt[3];
    if (!vec3_read_finite(origin, o) || !vec3_read_finite(dir, d) || !vec3_read_finite(center, cpt))
        return -1.0;
    if (!vec3_normalize_raw(d))
        return -1.0;

    double ox = o[0], oy = o[1], oz = o[2];
    double dx = d[0], dy = d[1], dz = d[2];
    double cx = cpt[0], cy = cpt[1], cz = cpt[2];
    if (!isfinite(radius) || radius < 0.0)
        return -1.0;
    radius = raycast3d_sanitize_distance(radius);

    double lx = ox - cx, ly = oy - cy, lz = oz - cz;
    double a = dx * dx + dy * dy + dz * dz;
    if (!isfinite(a) || a < EPSILON)
        return -1.0;
    double b = 2.0 * (lx * dx + ly * dy + lz * dz);
    double c = lx * lx + ly * ly + lz * lz - radius * radius;
    if (!isfinite(b) || !isfinite(c))
        return -1.0;
    if (c <= 0.0)
        return 0.0;

    double disc = b * b - 4.0 * a * c;
    if (!isfinite(disc) || disc < 0.0)
        return -1.0;

    double sqrt_disc = sqrt(disc);
    double t0 = (-b - sqrt_disc) / (2.0 * a);
    double t1 = (-b + sqrt_disc) / (2.0 * a);

    if (isfinite(t0) && t0 >= 0.0)
        return raycast3d_sanitize_hit_distance(t0);
    if (isfinite(t1) && t1 >= 0.0)
        return raycast3d_sanitize_hit_distance(t1);
    return -1.0;
}

/*==========================================================================
 * Ray-mesh intersection (retained BVH, AABB early-out, exact fallback)
 *=========================================================================*/

/// @brief Shared ray/mesh state for an intersection: the ray in both world and object space,
///        the optional model transform, and which space triangles are tested in.
typedef struct {
    /// Borrowed mesh payload whose geometry and retained BVH are queried.
    rt_mesh3d *m;
    /// Borrowed optional row-major model matrix; `NULL` means identity.
    const double *model;
    /// Whether a valid model transform was supplied.
    int has_transform;
    /// Whether the inverse transform supports local-space traversal.
    int use_object_space;
    /// Ray origin in object space, or world space for linear fallback.
    double obj_origin[3];
    /// Ray direction in object space, or world space for linear fallback.
    double obj_dir[3];
    /// Sanitized world-space ray origin.
    double world_origin[3];
    /// Normalized world-space ray direction.
    double world_dir[3];
} ray3d_mesh_ctx_t;

/// @brief Closest-triangle result from the Möller–Trumbore sweep.
typedef struct {
    /// Smallest parametric distance found in the active test space.
    double best_t;
    /// Deterministic source triangle index, or `-1` before a hit.
    int64_t best_tri;
    /// Vertex indices for the closest triangle.
    uint32_t best_i0, best_i1, best_i2;
    /// Closest hit position in mesh object space.
    double best_obj_point[3];
} ray3d_mesh_hit_t;

/// @brief Determine whether the prepared ray misses the mesh broad-phase AABB.
/// @details Transforms all eight local bounds corners when a singular model
///          transform forces world-space triangle testing.
/// @param ctx Borrowed prepared ray/mesh query state.
/// @return Nonzero when the ray misses the bounds; zero when narrow phase should run.
static int ray3d_mesh_misses_bounds(const ray3d_mesh_ctx_t *ctx) {
    rt_mesh3d *m = ctx->m;
    double bounds_min[3] = {m->aabb_min[0], m->aabb_min[1], m->aabb_min[2]};
    double bounds_max[3] = {m->aabb_max[0], m->aabb_max[1], m->aabb_max[2]};
    aabb3d_canonicalize_raw(bounds_min, bounds_max);
    if (ctx->has_transform && !ctx->use_object_space) {
        const double *model = ctx->model;
        double corners[8][3];
        double world_min[3] = {DBL_MAX, DBL_MAX, DBL_MAX};
        double world_max[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
        corners[0][0] = bounds_min[0];
        corners[0][1] = bounds_min[1];
        corners[0][2] = bounds_min[2];
        corners[1][0] = bounds_max[0];
        corners[1][1] = bounds_min[1];
        corners[1][2] = bounds_min[2];
        corners[2][0] = bounds_min[0];
        corners[2][1] = bounds_max[1];
        corners[2][2] = bounds_min[2];
        corners[3][0] = bounds_max[0];
        corners[3][1] = bounds_max[1];
        corners[3][2] = bounds_min[2];
        corners[4][0] = bounds_min[0];
        corners[4][1] = bounds_min[1];
        corners[4][2] = bounds_max[2];
        corners[5][0] = bounds_max[0];
        corners[5][1] = bounds_min[1];
        corners[5][2] = bounds_max[2];
        corners[6][0] = bounds_min[0];
        corners[6][1] = bounds_max[1];
        corners[6][2] = bounds_max[2];
        corners[7][0] = bounds_max[0];
        corners[7][1] = bounds_max[1];
        corners[7][2] = bounds_max[2];
        for (int i = 0; i < 8; i++) {
            double p[3];
            mat4_transform_point_raw(model, corners[i], p);
            vec3_sanitize_raw(p);
            for (int axis = 0; axis < 3; axis++) {
                if (p[axis] < world_min[axis])
                    world_min[axis] = p[axis];
                if (p[axis] > world_max[axis])
                    world_max[axis] = p[axis];
            }
        }
        aabb3d_canonicalize_raw(world_min, world_max);
        return rt_ray3d_intersect_aabb_raw(
                   ctx->world_origin, ctx->world_dir, world_min, world_max) < 0.0;
    }
    return rt_ray3d_intersect_aabb_raw(ctx->obj_origin, ctx->obj_dir, bounds_min, bounds_max) < 0.0;
}

/// @brief One retained scene-raycast BVH node over a contiguous triangle-index range.
/// @details Interior nodes have non-negative `left`/`right` and `count == 0`; leaves instead store
///          `[start, start + count)` into the mesh-owned `raycast_bvh_tri_indices` array.
typedef struct {
    /// Local-space minimum bounds.
    float min[3];
    /// Local-space maximum bounds.
    float max[3];
    /// Left child index for an interior node.
    int32_t left;
    /// Right child index for an interior node.
    int32_t right;
    /// First triangle-permutation slot for a leaf.
    int32_t start;
    /// Triangle count for a leaf; zero identifies an interior node.
    int32_t count;
} ray3d_mesh_bvh_node_t;

/// @brief Expand an AABB in place to include one finite three-component point.
/// @param mn Current minimum corner.
/// @param mx Current maximum corner.
/// @param point Point to include.
static void ray3d_bvh_expand(float *mn, float *mx, const float *point) {
    for (int axis = 0; axis < 3; ++axis) {
        if (point[axis] < mn[axis])
            mn[axis] = point[axis];
        if (point[axis] > mx[axis])
            mx[axis] = point[axis];
    }
}

/// @brief Validate one indexed triangle and compute its local AABB and centroid.
/// @details Non-finite vertices are excluded from the retained BVH, matching the narrow phase's
///          existing rule that such triangles cannot produce hits.
/// @param mesh Mesh containing the source index and vertex arrays.
/// @param triangle Triangle number (`index offset / 3`).
/// @param out_min Receives the local minimum corner.
/// @param out_max Receives the local maximum corner.
/// @param out_centroid Receives the arithmetic centroid.
/// @return Non-zero for a complete in-range finite triangle; zero otherwise.
static int ray3d_bvh_triangle_bounds(
    const rt_mesh3d *mesh, uint32_t triangle, float *out_min, float *out_max, float *out_centroid) {
    uint32_t vertex_count;
    uint32_t base;
    uint32_t indices[3];
    if (!mesh || !out_min || !out_max || !out_centroid || triangle > (UINT32_MAX - 2u) / 3u)
        return 0;
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    base = triangle * 3u;
    if (base + 2u >= rt_mesh3d_safe_index_count(mesh))
        return 0;
    indices[0] = mesh->indices[base + 0u];
    indices[1] = mesh->indices[base + 1u];
    indices[2] = mesh->indices[base + 2u];
    if (indices[0] >= vertex_count || indices[1] >= vertex_count || indices[2] >= vertex_count)
        return 0;
    for (int axis = 0; axis < 3; ++axis) {
        float a = mesh->vertices[indices[0]].pos[axis];
        float b = mesh->vertices[indices[1]].pos[axis];
        float c = mesh->vertices[indices[2]].pos[axis];
        if (!isfinite((double)a) || !isfinite((double)b) || !isfinite((double)c))
            return 0;
        out_min[axis] = fminf(a, fminf(b, c));
        out_max[axis] = fmaxf(a, fmaxf(b, c));
        out_centroid[axis] = (a + b + c) / 3.0f;
    }
    return 1;
}

/// @brief Recursively build a balanced retained BVH over a triangle-index subrange.
/// @details The widest centroid axis supplies a spatial pivot. Extremely imbalanced pivot splits
///          fall back to the range midpoint; child AABBs remain exact while tree depth stays
///          logarithmic even for adversarial centroid distributions.
/// @param mesh Source geometry.
/// @param nodes Preallocated node array.
/// @param node_count In/out number of initialized nodes.
/// @param node_capacity Capacity of @p nodes.
/// @param triangles Mutable triangle permutation.
/// @param start First triangle-permutation slot in this node.
/// @param count Number of slots in this node.
/// @return Node index, or -1 when inputs/capacity are invalid.
static int32_t ray3d_bvh_build_node(const rt_mesh3d *mesh,
                                    ray3d_mesh_bvh_node_t *nodes,
                                    int32_t *node_count,
                                    int32_t node_capacity,
                                    uint32_t *triangles,
                                    int32_t start,
                                    int32_t count) {
    ray3d_mesh_bvh_node_t *node;
    float centroid_min[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float centroid_max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    int32_t node_index;
    int split_axis = 0;
    if (!mesh || !nodes || !node_count || !triangles || start < 0 || count <= 0 ||
        *node_count < 0 || *node_count >= node_capacity)
        return -1;
    node_index = (*node_count)++;
    node = &nodes[node_index];
    node->min[0] = node->min[1] = node->min[2] = FLT_MAX;
    node->max[0] = node->max[1] = node->max[2] = -FLT_MAX;
    node->left = -1;
    node->right = -1;
    node->start = start;
    node->count = count;
    for (int32_t i = start; i < start + count; ++i) {
        float tri_min[3];
        float tri_max[3];
        float centroid[3];
        if (!ray3d_bvh_triangle_bounds(mesh, triangles[i], tri_min, tri_max, centroid))
            return -1;
        ray3d_bvh_expand(node->min, node->max, tri_min);
        ray3d_bvh_expand(node->min, node->max, tri_max);
        ray3d_bvh_expand(centroid_min, centroid_max, centroid);
    }
    if (count <= 8)
        return node_index;
    {
        float extent_x = centroid_max[0] - centroid_min[0];
        float extent_y = centroid_max[1] - centroid_min[1];
        float extent_z = centroid_max[2] - centroid_min[2];
        if (extent_y > extent_x && extent_y >= extent_z)
            split_axis = 1;
        else if (extent_z > extent_x && extent_z >= extent_y)
            split_axis = 2;
    }
    {
        float pivot = 0.5f * (centroid_min[split_axis] + centroid_max[split_axis]);
        int32_t lo = start;
        int32_t hi = start + count - 1;
        int32_t left_count;
        while (lo <= hi) {
            float tri_min[3];
            float tri_max[3];
            float centroid[3];
            if (!ray3d_bvh_triangle_bounds(mesh, triangles[lo], tri_min, tri_max, centroid))
                return -1;
            if (centroid[split_axis] < pivot) {
                ++lo;
            } else {
                uint32_t tmp = triangles[lo];
                triangles[lo] = triangles[hi];
                triangles[hi] = tmp;
                --hi;
            }
        }
        left_count = lo - start;
        if (left_count < count / 4 || left_count > count - count / 4)
            left_count = count / 2;
        node->left = ray3d_bvh_build_node(
            mesh, nodes, node_count, node_capacity, triangles, start, left_count);
        node->right = ray3d_bvh_build_node(mesh,
                                           nodes,
                                           node_count,
                                           node_capacity,
                                           triangles,
                                           start + left_count,
                                           count - left_count);
        if (node->left < 0 || node->right < 0)
            return -1;
        node->count = 0;
    }
    return node_index;
}

/// @brief Build or reuse the scene-raycast BVH for the mesh's current geometry revision.
/// @details Valid finite triangles are gathered transactionally, then a balanced local-space tree
///          is installed only after construction succeeds. Repeated queries on an unchanged mesh
///          perform no allocation or triangle-bound rebuild. Corrupt triangles remain skippable,
///          preserving the legacy linear query's tolerant behavior.
/// @param mesh Mesh whose retained raycast acceleration should be made current.
/// @return Non-zero when a non-empty BVH is available; zero on empty geometry or allocation error.
static int ray3d_mesh_bvh_rebuild(rt_mesh3d *mesh) {
    ray3d_mesh_bvh_node_t *nodes = NULL;
    uint32_t *triangles = NULL;
    uint32_t triangle_total;
    int32_t triangle_count = 0;
    int32_t node_capacity;
    int32_t node_count = 0;
    if (!mesh)
        return 0;
    if (mesh->raycast_bvh_revision == mesh->geometry_revision)
        return mesh->raycast_bvh_nodes && mesh->raycast_bvh_node_count > 0;
    triangle_total = rt_mesh3d_safe_index_count(mesh) / 3u;
    if (triangle_total == 0 || triangle_total > (uint32_t)(INT32_MAX / 2))
        return 0;
    triangles = (uint32_t *)malloc((size_t)triangle_total * sizeof(*triangles));
    if (!triangles)
        return 0;
    for (uint32_t triangle = 0; triangle < triangle_total; ++triangle) {
        float tri_min[3];
        float tri_max[3];
        float centroid[3];
        if (ray3d_bvh_triangle_bounds(mesh, triangle, tri_min, tri_max, centroid))
            triangles[triangle_count++] = triangle;
    }
    if (triangle_count == 0) {
        free(triangles);
        free(mesh->raycast_bvh_nodes);
        free(mesh->raycast_bvh_tri_indices);
        mesh->raycast_bvh_nodes = NULL;
        mesh->raycast_bvh_tri_indices = NULL;
        mesh->raycast_bvh_node_count = 0;
        mesh->raycast_bvh_tri_count = 0;
        mesh->raycast_bvh_revision = mesh->geometry_revision;
        return 0;
    }
    node_capacity = triangle_count * 2;
    nodes = (ray3d_mesh_bvh_node_t *)calloc((size_t)node_capacity, sizeof(*nodes));
    if (!nodes) {
        free(triangles);
        return 0;
    }
    if (ray3d_bvh_build_node(
            mesh, nodes, &node_count, node_capacity, triangles, 0, triangle_count) < 0) {
        free(nodes);
        free(triangles);
        return 0;
    }
    free(mesh->raycast_bvh_nodes);
    free(mesh->raycast_bvh_tri_indices);
    mesh->raycast_bvh_nodes = nodes;
    mesh->raycast_bvh_tri_indices = triangles;
    mesh->raycast_bvh_revision = mesh->geometry_revision;
    mesh->raycast_bvh_node_count = node_count;
    mesh->raycast_bvh_tri_count = triangle_count;
    if (mesh->raycast_bvh_rebuild_count != UINT64_MAX)
        mesh->raycast_bvh_rebuild_count++;
    return 1;
}

/// @brief Test one mesh triangle and update the deterministic closest-hit accumulator.
/// @details Triangle probe instrumentation is incremented before validation. Equal-distance hits
///          resolve to the lower source triangle index, preserving the result of the former
///          ascending linear scan regardless of BVH traversal order.
/// @param ctx Prepared ray/mesh state.
/// @param triangle Source triangle number.
/// @param out In/out closest-hit record.
static void ray3d_consider_mesh_triangle(const ray3d_mesh_ctx_t *ctx,
                                         uint32_t triangle,
                                         ray3d_mesh_hit_t *out) {
    rt_mesh3d *mesh = ctx ? ctx->m : NULL;
    uint32_t base;
    uint32_t vertex_count;
    uint32_t i0;
    uint32_t i1;
    uint32_t i2;
    double a[3];
    double b[3];
    double c[3];
    if (!mesh || !out || triangle > (UINT32_MAX - 2u) / 3u)
        return;
    if (mesh->raycast_last_triangle_probe_count != UINT64_MAX)
        mesh->raycast_last_triangle_probe_count++;
    base = triangle * 3u;
    if (base + 2u >= rt_mesh3d_safe_index_count(mesh))
        return;
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    i0 = mesh->indices[base + 0u];
    i1 = mesh->indices[base + 1u];
    i2 = mesh->indices[base + 2u];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
        return;
    for (int axis = 0; axis < 3; ++axis) {
        a[axis] = mesh->vertices[i0].pos[axis];
        b[axis] = mesh->vertices[i1].pos[axis];
        c[axis] = mesh->vertices[i2].pos[axis];
    }
    if (!vec3_is_finite_raw(a) || !vec3_is_finite_raw(b) || !vec3_is_finite_raw(c))
        return;
    vec3_sanitize_raw(a);
    vec3_sanitize_raw(b);
    vec3_sanitize_raw(c);
    if (ctx->has_transform && !ctx->use_object_space) {
        const double *model = ctx->model;
        mat4_transform_point_raw(model, a, a);
        mat4_transform_point_raw(model, b, b);
        mat4_transform_point_raw(model, c, c);
    }
    {
        double e1x = b[0] - a[0];
        double e1y = b[1] - a[1];
        double e1z = b[2] - a[2];
        double e2x = c[0] - a[0];
        double e2y = c[1] - a[1];
        double e2z = c[2] - a[2];
        double px = ctx->obj_dir[1] * e2z - ctx->obj_dir[2] * e2y;
        double py = ctx->obj_dir[2] * e2x - ctx->obj_dir[0] * e2z;
        double pz = ctx->obj_dir[0] * e2y - ctx->obj_dir[1] * e2x;
        double det = e1x * px + e1y * py + e1z * pz;
        double inv_det;
        double tvx;
        double tvy;
        double tvz;
        double u;
        double qx;
        double qy;
        double qz;
        double v;
        double t;
        if (!isfinite(px) || !isfinite(py) || !isfinite(pz) || !isfinite(det) ||
            fabs(det) < EPSILON)
            return;
        inv_det = 1.0 / det;
        tvx = ctx->obj_origin[0] - a[0];
        tvy = ctx->obj_origin[1] - a[1];
        tvz = ctx->obj_origin[2] - a[2];
        u = (tvx * px + tvy * py + tvz * pz) * inv_det;
        if (!isfinite(u) || u < 0.0 || u > 1.0)
            return;
        qx = tvy * e1z - tvz * e1y;
        qy = tvz * e1x - tvx * e1z;
        qz = tvx * e1y - tvy * e1x;
        v = (ctx->obj_dir[0] * qx + ctx->obj_dir[1] * qy + ctx->obj_dir[2] * qz) * inv_det;
        if (!isfinite(v) || v < 0.0 || u + v > 1.0)
            return;
        t = (e2x * qx + e2y * qy + e2z * qz) * inv_det;
        if (!isfinite(t) || t < 0.0 ||
            !(t < out->best_t || (t == out->best_t && (int64_t)triangle < out->best_tri)))
            return;
        out->best_t = t;
        out->best_tri = (int64_t)triangle;
        out->best_i0 = i0;
        out->best_i1 = i1;
        out->best_i2 = i2;
        out->best_obj_point[0] = ctx->obj_origin[0] + ctx->obj_dir[0] * t;
        out->best_obj_point[1] = ctx->obj_origin[1] + ctx->obj_dir[1] * t;
        out->best_obj_point[2] = ctx->obj_origin[2] + ctx->obj_dir[2] * t;
        vec3_sanitize_raw(out->best_obj_point);
    }
}

/// @brief Return the ray entry distance for one retained BVH node, or -1 on a miss.
/// @param ctx Prepared object-space ray state.
/// @param node Node whose local AABB should be tested.
/// @return Non-negative object-space entry distance, or `-1` on a miss or invalid input.
static double ray3d_bvh_node_entry(const ray3d_mesh_ctx_t *ctx, const ray3d_mesh_bvh_node_t *node) {
    double mn[3];
    double mx[3];
    if (!ctx || !node)
        return -1.0;
    for (int axis = 0; axis < 3; ++axis) {
        mn[axis] = node->min[axis];
        mx[axis] = node->max[axis];
    }
    return rt_ray3d_intersect_aabb_raw(ctx->obj_origin, ctx->obj_dir, mn, mx);
}

/// @brief Traverse one retained BVH subtree near-first and test candidate leaf triangles.
/// @details Child entry distances select traversal order. The current closest triangle distance
///          prunes farther nodes, while equal-entry child nodes use node index as a deterministic
///          tie-break.
/// @param ctx Prepared object-space ray/mesh state.
/// @param nodes Retained node array.
/// @param node_index Subtree root index.
/// @param out In/out closest-hit record.
static void ray3d_bvh_traverse(const ray3d_mesh_ctx_t *ctx,
                               const ray3d_mesh_bvh_node_t *nodes,
                               int32_t node_index,
                               ray3d_mesh_hit_t *out) {
    const ray3d_mesh_bvh_node_t *node;
    double node_t;
    if (!ctx || !ctx->m || !nodes || !out || node_index < 0 ||
        node_index >= ctx->m->raycast_bvh_node_count)
        return;
    node = &nodes[node_index];
    node_t = ray3d_bvh_node_entry(ctx, node);
    if (node_t < 0.0 || node_t > out->best_t)
        return;
    if (node->count > 0) {
        for (int32_t i = node->start; i < node->start + node->count; ++i) {
            if (i >= 0 && i < ctx->m->raycast_bvh_tri_count)
                ray3d_consider_mesh_triangle(ctx, ctx->m->raycast_bvh_tri_indices[i], out);
        }
        return;
    }
    {
        int32_t first = node->left;
        int32_t second = node->right;
        double first_t = first >= 0 ? ray3d_bvh_node_entry(ctx, &nodes[first]) : -1.0;
        double second_t = second >= 0 ? ray3d_bvh_node_entry(ctx, &nodes[second]) : -1.0;
        if (second_t >= 0.0 &&
            (first_t < 0.0 || second_t < first_t || (second_t == first_t && second < first))) {
            int32_t tmp_index = first;
            double tmp_t = first_t;
            first = second;
            first_t = second_t;
            second = tmp_index;
            second_t = tmp_t;
        }
        if (first_t >= 0.0 && first_t <= out->best_t)
            ray3d_bvh_traverse(ctx, nodes, first, out);
        if (second_t >= 0.0 && second_t <= out->best_t)
            ray3d_bvh_traverse(ctx, nodes, second, out);
    }
}

/// @brief Find the nearest triangle using a retained local-space BVH where possible.
/// @details Identity and invertible-transform queries reuse the mesh BVH. Singular transforms
///          cannot transform the ray to local space, so they preserve the exact world-space linear
///          fallback. Allocation/build failure also degrades safely to the legacy sweep.
/// @param ctx Prepared ray/mesh state.
/// @param out Receives the deterministic closest triangle and intersection data.
/// @return Non-zero when a triangle was hit; zero otherwise.
static int ray3d_find_closest_triangle(const ray3d_mesh_ctx_t *ctx, ray3d_mesh_hit_t *out) {
    rt_mesh3d *mesh = ctx ? ctx->m : NULL;
    uint32_t triangle_count;
    if (!mesh || !out)
        return 0;
    out->best_t = DBL_MAX;
    out->best_tri = -1;
    out->best_i0 = out->best_i1 = out->best_i2 = 0;
    out->best_obj_point[0] = out->best_obj_point[1] = out->best_obj_point[2] = 0.0;
    mesh->raycast_last_triangle_probe_count = 0;
    if ((!ctx->has_transform || ctx->use_object_space) && ray3d_mesh_bvh_rebuild(mesh)) {
        ray3d_bvh_traverse(ctx, (const ray3d_mesh_bvh_node_t *)mesh->raycast_bvh_nodes, 0, out);
        return out->best_tri >= 0;
    }
    triangle_count = rt_mesh3d_safe_index_count(mesh) / 3u;
    for (uint32_t triangle = 0; triangle < triangle_count; ++triangle)
        ray3d_consider_mesh_triangle(ctx, triangle, out);
    return out->best_tri >= 0;
}

/// @brief Build the RayHit3D result for a found triangle: world-space point, face normal, distance.
/// @param ctx Borrowed prepared ray/mesh state.
/// @param hd Borrowed validated closest-triangle record.
/// @return The hit object, or NULL on allocation failure.
static void *ray3d_build_mesh_hit(const ray3d_mesh_ctx_t *ctx, const ray3d_mesh_hit_t *hd) {
    rt_mesh3d *m = ctx->m;
    rt_rayhit3d *hit =
        (rt_rayhit3d *)rt_obj_new_i64(RT_G3D_RAYHIT3D_CLASS_ID, (int64_t)sizeof(rt_rayhit3d));
    double best_normal[3];
    if (!hit)
        return NULL;

    {
        double a[3] = {m->vertices[hd->best_i0].pos[0],
                       m->vertices[hd->best_i0].pos[1],
                       m->vertices[hd->best_i0].pos[2]};
        double b[3] = {m->vertices[hd->best_i1].pos[0],
                       m->vertices[hd->best_i1].pos[1],
                       m->vertices[hd->best_i1].pos[2]};
        double c[3] = {m->vertices[hd->best_i2].pos[0],
                       m->vertices[hd->best_i2].pos[1],
                       m->vertices[hd->best_i2].pos[2]};
        vec3_sanitize_raw(a);
        vec3_sanitize_raw(b);
        vec3_sanitize_raw(c);
        if (ctx->has_transform) {
            const double *model = ctx->model;
            mat4_transform_point_raw(model, a, a);
            mat4_transform_point_raw(model, b, b);
            mat4_transform_point_raw(model, c, c);
            if (ctx->use_object_space)
                mat4_transform_point_raw(model, hd->best_obj_point, hit->point);
            else {
                hit->point[0] = ctx->world_origin[0] + ctx->world_dir[0] * hd->best_t;
                hit->point[1] = ctx->world_origin[1] + ctx->world_dir[1] * hd->best_t;
                hit->point[2] = ctx->world_origin[2] + ctx->world_dir[2] * hd->best_t;
                vec3_sanitize_raw(hit->point);
            }
        } else {
            hit->point[0] = ctx->world_origin[0] + ctx->world_dir[0] * hd->best_t;
            hit->point[1] = ctx->world_origin[1] + ctx->world_dir[1] * hd->best_t;
            hit->point[2] = ctx->world_origin[2] + ctx->world_dir[2] * hd->best_t;
            vec3_sanitize_raw(hit->point);
        }

        {
            double e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
            double e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
            best_normal[0] = e1y * e2z - e1z * e2y;
            best_normal[1] = e1z * e2x - e1x * e2z;
            best_normal[2] = e1x * e2y - e1y * e2x;
        }
    }

    {
        double nlen = sqrt(best_normal[0] * best_normal[0] + best_normal[1] * best_normal[1] +
                           best_normal[2] * best_normal[2]);
        if (isfinite(nlen) && nlen > 1e-8) {
            best_normal[0] /= nlen;
            best_normal[1] /= nlen;
            best_normal[2] /= nlen;
        } else {
            best_normal[0] = 0.0;
            best_normal[1] = 1.0;
            best_normal[2] = 0.0;
        }
    }
    vec3_normalize_or_up_raw(best_normal);

    {
        double dir_len_sq = ctx->world_dir[0] * ctx->world_dir[0] +
                            ctx->world_dir[1] * ctx->world_dir[1] +
                            ctx->world_dir[2] * ctx->world_dir[2];
        hit->distance = dir_len_sq > 1e-12
                            ? (((hit->point[0] - ctx->world_origin[0]) * ctx->world_dir[0] +
                                (hit->point[1] - ctx->world_origin[1]) * ctx->world_dir[1] +
                                (hit->point[2] - ctx->world_origin[2]) * ctx->world_dir[2]) /
                               dir_len_sq)
                            : 0.0;
    }
    hit->distance = raycast3d_sanitize_hit_distance(hit->distance);
    vec3_sanitize_raw(hit->point);
    hit->normal[0] = best_normal[0];
    hit->normal[1] = best_normal[1];
    hit->normal[2] = best_normal[2];
    hit->triangle_index = hd->best_tri;
    return hit;
}

/// @brief Box a RayHit3D from raw hit data for scene-level precise queries.
/// @details Keeps the RayHit3D payload layout private to this file while the
///          scene queries in rt_scene3d_query.c report triangle hits found
///          through `rt_raycast3d_mesh_hit_raw`.
/// @param distance Sanitized non-negative Euclidean world hit distance.
/// @param point Borrowed world-space hit point.
/// @param normal Borrowed normalized world-space face normal.
/// @param triangle Winning triangle index within the hit mesh.
/// @return New RayHit3D object, or `NULL` on invalid input or allocation failure.
void *rt_raycast3d_build_hit(double distance,
                             const double point[3],
                             const double normal[3],
                             int64_t triangle) {
    rt_rayhit3d *hit;
    if (!point || !normal)
        return NULL;
    hit = (rt_rayhit3d *)rt_obj_new_i64(RT_G3D_RAYHIT3D_CLASS_ID, (int64_t)sizeof(rt_rayhit3d));
    if (!hit)
        return NULL;
    hit->distance = raycast3d_sanitize_hit_distance(distance);
    hit->point[0] = point[0];
    hit->point[1] = point[1];
    hit->point[2] = point[2];
    hit->normal[0] = normal[0];
    hit->normal[1] = normal[1];
    hit->normal[2] = normal[2];
    vec3_sanitize_raw(hit->point);
    vec3_normalize_or_up_raw(hit->normal);
    hit->triangle_index = triangle;
    return hit;
}

/// @brief Prepare the ray/mesh context and run the shared narrow phase.
/// @details Sanitizes and normalizes the world ray, validates the mesh
///          geometry, converts the ray into object space when the model
///          matrix inverts (world-space exact fallback otherwise), applies
///          the broad-phase AABB early-out, then finds the closest triangle.
/// @param world_origin Borrowed finite world-space ray origin.
/// @param world_dir Borrowed world-space direction, normalized internally.
/// @param m Borrowed validated mesh payload.
/// @param model Optional borrowed row-major model matrix; `NULL` = identity.
/// @param ctx Output prepared query context.
/// @param hit Output closest-triangle record.
/// @return Nonzero when a triangle was hit and @p ctx / @p hit are valid.
static int ray3d_mesh_query_raw(const double world_origin[3],
                                const double world_dir[3],
                                rt_mesh3d *m,
                                const double *model,
                                ray3d_mesh_ctx_t *ctx,
                                ray3d_mesh_hit_t *hit) {
    double inv_model[16];
    if (!world_origin || !world_dir || !m || !ctx || !hit)
        return 0;
    memset(ctx, 0, sizeof(*ctx));
    ctx->world_origin[0] = world_origin[0];
    ctx->world_origin[1] = world_origin[1];
    ctx->world_origin[2] = world_origin[2];
    ctx->world_dir[0] = world_dir[0];
    ctx->world_dir[1] = world_dir[1];
    ctx->world_dir[2] = world_dir[2];
    if (!vec3_is_finite_raw(ctx->world_origin) || !vec3_is_finite_raw(ctx->world_dir))
        return 0;
    m->raycast_last_triangle_probe_count = 0;
    if (!vec3_normalize_raw(ctx->world_dir))
        return 0;
    if (model && !mat4d_is_finite(model))
        return 0;
    rt_mesh3d_repair_geometry_counts(m);
    if (rt_mesh3d_safe_vertex_count(m) == 0 || rt_mesh3d_safe_index_count(m) < 3)
        return 0;

    ctx->m = m;
    ctx->model = model;
    ctx->has_transform = (model != NULL);

    rt_mesh3d_refresh_bounds(m);

    if (ctx->has_transform) {
        if (mat4d_invert(model, inv_model) == 0) {
            double world_target[3] = {ctx->world_origin[0] + ctx->world_dir[0],
                                      ctx->world_origin[1] + ctx->world_dir[1],
                                      ctx->world_origin[2] + ctx->world_dir[2]};
            double obj_target[3];
            mat4_transform_point_raw(inv_model, ctx->world_origin, ctx->obj_origin);
            mat4_transform_point_raw(inv_model, world_target, obj_target);
            ctx->obj_dir[0] = obj_target[0] - ctx->obj_origin[0];
            ctx->obj_dir[1] = obj_target[1] - ctx->obj_origin[1];
            ctx->obj_dir[2] = obj_target[2] - ctx->obj_origin[2];
            vec3_sanitize_raw(ctx->obj_origin);
            vec3_sanitize_raw(ctx->obj_dir);
            if (vec3_is_finite_raw(ctx->obj_origin) && vec3_is_finite_raw(ctx->obj_dir)) {
                double dir_len_sq = ctx->obj_dir[0] * ctx->obj_dir[0] +
                                    ctx->obj_dir[1] * ctx->obj_dir[1] +
                                    ctx->obj_dir[2] * ctx->obj_dir[2];
                ctx->use_object_space = isfinite(dir_len_sq) && dir_len_sq >= EPSILON * EPSILON;
            }
        }
    }
    if (!ctx->use_object_space) {
        ctx->obj_origin[0] = ctx->world_origin[0];
        ctx->obj_origin[1] = ctx->world_origin[1];
        ctx->obj_origin[2] = ctx->world_origin[2];
        ctx->obj_dir[0] = ctx->world_dir[0];
        ctx->obj_dir[1] = ctx->world_dir[1];
        ctx->obj_dir[2] = ctx->world_dir[2];
    }
    vec3_sanitize_raw(ctx->obj_origin);
    vec3_sanitize_raw(ctx->obj_dir);
    {
        double obj_dir_len_sq = ctx->obj_dir[0] * ctx->obj_dir[0] +
                                ctx->obj_dir[1] * ctx->obj_dir[1] +
                                ctx->obj_dir[2] * ctx->obj_dir[2];
        if (!isfinite(obj_dir_len_sq) || obj_dir_len_sq < EPSILON * EPSILON)
            return 0;
    }

    if (ray3d_mesh_misses_bounds(ctx))
        return 0;
    return ray3d_find_closest_triangle(ctx, hit);
}

/// @brief Intersect a world-space ray against a transformed Mesh3D.
/// @details Performs an AABB early-out, then lazily builds or reuses a
///          local-space BVH keyed by geometry revision. Singular transforms and
///          BVH allocation failures preserve an exact world-space linear sweep.
/// @param origin Borrowed Vec3 ray origin in world space.
/// @param dir Borrowed Vec3 direction, normalized internally.
/// @param mesh_obj Borrowed Mesh3D handle to test.
/// @param transform_obj Optional borrowed Mat4 model transform; `NULL` means identity.
/// @return The nearest RayHit3D object, or NULL on a miss or invalid/degenerate inputs.
void *rt_ray3d_intersect_mesh(void *origin, void *dir, void *mesh_obj, void *transform_obj) {
    ray3d_mesh_ctx_t ctx;
    ray3d_mesh_hit_t hit_data;
    double world_origin[3];
    double world_dir[3];
    rt_mesh3d *m = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
    mat4_impl *transform = NULL;

    if (!vec3_read_finite(origin, world_origin) || !vec3_read_finite(dir, world_dir) || !m)
        return NULL;
    if (transform_obj) {
        transform = raycast3d_mat4_checked(transform_obj);
        if (!transform)
            return NULL;
    }
    if (!ray3d_mesh_query_raw(
            world_origin, world_dir, m, transform ? transform->m : NULL, &ctx, &hit_data))
        return NULL;
    return ray3d_build_mesh_hit(&ctx, &hit_data);
}

/// @brief Allocation-free triangle-accurate mesh hit for scene-level queries.
/// @details Runs the same broad phase, BVH narrow phase, and fallbacks as
///          `rt_ray3d_intersect_mesh` but takes raw doubles and reports the
///          hit without building a RayHit3D. The world-space face normal is
///          derived from the winning triangle exactly as the boxed query does.
/// @param origin Borrowed finite world-space ray origin.
/// @param dir Borrowed world-space direction, normalized internally.
/// @param mesh Borrowed validated mesh payload.
/// @param model Optional borrowed row-major world matrix; `NULL` = identity.
/// @param max_distance Non-negative Euclidean hit cap; negative means uncapped.
/// @param out_distance Optional output Euclidean world hit distance.
/// @param out_triangle Optional output winning triangle index.
/// @param out_point Optional output world-space hit point.
/// @param out_normal Optional output normalized world-space face normal.
/// @return Nonzero when a triangle hit within range was found.
int rt_raycast3d_mesh_hit_raw(const double origin[3],
                              const double dir[3],
                              rt_mesh3d *mesh,
                              const double *model,
                              double max_distance,
                              double *out_distance,
                              int64_t *out_triangle,
                              double *out_point,
                              double *out_normal) {
    ray3d_mesh_ctx_t ctx;
    ray3d_mesh_hit_t hit;
    double point[3];
    double normal[3];
    double distance;

    if (!ray3d_mesh_query_raw(origin, dir, mesh, model, &ctx, &hit))
        return 0;

    if (ctx.has_transform && ctx.use_object_space) {
        mat4_transform_point_raw(ctx.model, hit.best_obj_point, point);
    } else {
        point[0] = ctx.world_origin[0] + ctx.world_dir[0] * hit.best_t;
        point[1] = ctx.world_origin[1] + ctx.world_dir[1] * hit.best_t;
        point[2] = ctx.world_origin[2] + ctx.world_dir[2] * hit.best_t;
    }
    vec3_sanitize_raw(point);

    distance = (point[0] - ctx.world_origin[0]) * ctx.world_dir[0] +
               (point[1] - ctx.world_origin[1]) * ctx.world_dir[1] +
               (point[2] - ctx.world_origin[2]) * ctx.world_dir[2];
    distance = raycast3d_sanitize_hit_distance(distance);
    if (distance < 0.0)
        return 0;
    if (max_distance >= 0.0 && distance > max_distance)
        return 0;

    if (out_normal) {
        rt_mesh3d *m = ctx.m;
        double a[3] = {m->vertices[hit.best_i0].pos[0],
                       m->vertices[hit.best_i0].pos[1],
                       m->vertices[hit.best_i0].pos[2]};
        double b[3] = {m->vertices[hit.best_i1].pos[0],
                       m->vertices[hit.best_i1].pos[1],
                       m->vertices[hit.best_i1].pos[2]};
        double c[3] = {m->vertices[hit.best_i2].pos[0],
                       m->vertices[hit.best_i2].pos[1],
                       m->vertices[hit.best_i2].pos[2]};
        vec3_sanitize_raw(a);
        vec3_sanitize_raw(b);
        vec3_sanitize_raw(c);
        if (ctx.has_transform) {
            mat4_transform_point_raw(ctx.model, a, a);
            mat4_transform_point_raw(ctx.model, b, b);
            mat4_transform_point_raw(ctx.model, c, c);
        }
        {
            double e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
            double e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
            normal[0] = e1y * e2z - e1z * e2y;
            normal[1] = e1z * e2x - e1x * e2z;
            normal[2] = e1x * e2y - e1y * e2x;
        }
        vec3_normalize_or_up_raw(normal);
        out_normal[0] = normal[0];
        out_normal[1] = normal[1];
        out_normal[2] = normal[2];
    }
    if (out_distance)
        *out_distance = distance;
    if (out_triangle)
        *out_triangle = hit.best_tri;
    if (out_point) {
        out_point[0] = point[0];
        out_point[1] = point[1];
        out_point[2] = point[2];
    }
    return 1;
}

/*==========================================================================
 * AABB-AABB collision
 *=========================================================================*/

/// @brief Test whether two axis-aligned bounding boxes overlap.
/// @param min_a Borrowed Vec3 minimum corner of box A.
/// @param max_a Borrowed Vec3 maximum corner of box A.
/// @param min_b Borrowed Vec3 minimum corner of box B.
/// @param max_b Borrowed Vec3 maximum corner of box B.
/// @return 1 if they overlap on all three axes, 0 otherwise.
int8_t rt_aabb3d_overlaps(void *min_a, void *max_a, void *min_b, void *max_b) {
    double amin[3], amax[3], bmin[3], bmax[3];
    if (!vec3_read_finite(min_a, amin) || !vec3_read_finite(max_a, amax) ||
        !vec3_read_finite(min_b, bmin) || !vec3_read_finite(max_b, bmax))
        return 0;
    aabb3d_canonicalize_raw(amin, amax);
    aabb3d_canonicalize_raw(bmin, bmax);
    return (amin[0] <= bmax[0] && amax[0] >= bmin[0] && amin[1] <= bmax[1] && amax[1] >= bmin[1] &&
            amin[2] <= bmax[2] && amax[2] >= bmin[2])
               ? 1
               : 0;
}

/// @brief Compute the minimum-axis penetration vector to separate two overlapping AABBs.
/// @details Finds the axis with the smallest overlap and returns a push vector
///          along that axis. Returns (0,0,0) if the boxes do not overlap.
/// @param min_a Borrowed Vec3 minimum corner of box A.
/// @param max_a Borrowed Vec3 maximum corner of box A.
/// @param min_b Borrowed Vec3 minimum corner of box B.
/// @param max_b Borrowed Vec3 maximum corner of box B.
/// @return New Vec3 that pushes A out of B along the minimum-overlap axis, or zero.
void *rt_aabb3d_penetration(void *min_a, void *max_a, void *min_b, void *max_b) {
    double amin[3], amax[3], bmin[3], bmax[3];
    if (!vec3_read_finite(min_a, amin) || !vec3_read_finite(max_a, amax) ||
        !vec3_read_finite(min_b, bmin) || !vec3_read_finite(max_b, bmax))
        return rt_vec3_new(0, 0, 0);
    aabb3d_canonicalize_raw(amin, amax);
    aabb3d_canonicalize_raw(bmin, bmax);

    /* No overlap → zero penetration */
    if (amin[0] > bmax[0] || amax[0] < bmin[0] || amin[1] > bmax[1] || amax[1] < bmin[1] ||
        amin[2] > bmax[2] || amax[2] < bmin[2])
        return rt_vec3_new(0, 0, 0);

    /* Compute overlap on each axis */
    double ox = (amax[0] < bmax[0] ? amax[0] - bmin[0] : bmax[0] - amin[0]);
    double oy = (amax[1] < bmax[1] ? amax[1] - bmin[1] : bmax[1] - amin[1]);
    double oz = (amax[2] < bmax[2] ? amax[2] - bmin[2] : bmax[2] - amin[2]);

    /* Push out on the axis of minimum overlap */
    double ax = fabs(ox), ay = fabs(oy), az = fabs(oz);
    double cax = (amin[0] + amax[0]) * 0.5;
    double cay = (amin[1] + amax[1]) * 0.5;
    double caz = (amin[2] + amax[2]) * 0.5;
    double cb;

    if (ax <= ay && ax <= az) {
        cb = (bmin[0] + bmax[0]) * 0.5;
        return vec3_new_sanitized(cax < cb ? -ox : ox, 0, 0);
    } else if (ay <= az) {
        cb = (bmin[1] + bmax[1]) * 0.5;
        return vec3_new_sanitized(0, cay < cb ? -oy : oy, 0);
    } else {
        cb = (bmin[2] + bmax[2]) * 0.5;
        return vec3_new_sanitized(0, 0, caz < cb ? -oz : oz);
    }
}

/*==========================================================================
 * RayHit3D accessors
 *=========================================================================*/

/// @brief Get the distance along the ray to the hit point.
/// @param hit Borrowed RayHit3D handle.
/// @return Sanitized non-negative distance, or `-1` for an invalid handle.
double rt_ray3d_hit_distance(void *hit) {
    rt_rayhit3d *h = (rt_rayhit3d *)rt_g3d_checked_or_null(hit, RT_G3D_RAYHIT3D_CLASS_ID);
    return h ? raycast3d_sanitize_hit_distance(h->distance) : -1.0;
}

/// @brief Get the world-space position of the hit point as a new Vec3.
/// @param hit Borrowed RayHit3D handle.
/// @return New sanitized Vec3, or the zero vector for an invalid handle.
void *rt_ray3d_hit_point(void *hit) {
    rt_rayhit3d *h = (rt_rayhit3d *)rt_g3d_checked_or_null(hit, RT_G3D_RAYHIT3D_CLASS_ID);
    if (!h)
        return rt_vec3_new(0, 0, 0);
    return vec3_new_sanitized(h->point[0], h->point[1], h->point[2]);
}

/// @brief Get the surface normal at the hit point as a new Vec3.
/// @param hit Borrowed RayHit3D handle.
/// @return New unit Vec3 normal, defaulting to positive Y for an invalid handle.
void *rt_ray3d_hit_normal(void *hit) {
    rt_rayhit3d *h = (rt_rayhit3d *)rt_g3d_checked_or_null(hit, RT_G3D_RAYHIT3D_CLASS_ID);
    if (!h)
        return rt_vec3_new(0, 1, 0);
    {
        double n[3] = {h->normal[0], h->normal[1], h->normal[2]};
        vec3_normalize_or_up_raw(n);
        return rt_vec3_new(n[0], n[1], n[2]);
    }
}

/// @brief Get the index of the triangle that was hit (-1 if no hit).
/// @param hit Borrowed RayHit3D handle.
/// @return Source triangle index, or `-1` for an invalid handle.
int64_t rt_ray3d_hit_triangle(void *hit) {
    rt_rayhit3d *h = (rt_rayhit3d *)rt_g3d_checked_or_null(hit, RT_G3D_RAYHIT3D_CLASS_ID);
    return h ? h->triangle_index : -1;
}

/*==========================================================================
 * Shape-shape collision primitives (for Physics3D)
 *=========================================================================*/

/// @brief Test whether two spheres overlap (distance < sum of radii).
/// @param center_a Borrowed Vec3 center of sphere A.
/// @param radius_a Radius of sphere A; invalid values sanitize to zero.
/// @param center_b Borrowed Vec3 center of sphere B.
/// @param radius_b Radius of sphere B; invalid values sanitize to zero.
/// @return Nonzero when the closed sphere volumes overlap or touch.
int8_t rt_sphere3d_overlaps(void *center_a, double radius_a, void *center_b, double radius_b) {
    double ca[3], cb[3];
    if (!vec3_read_finite(center_a, ca) || !vec3_read_finite(center_b, cb))
        return 0;
    radius_a = raycast3d_sanitize_distance(radius_a);
    radius_b = raycast3d_sanitize_distance(radius_b);
    double dx = cb[0] - ca[0];
    double dy = cb[1] - ca[1];
    double dz = cb[2] - ca[2];
    double dist_sq = dx * dx + dy * dy + dz * dz;
    double r_sum = radius_a + radius_b;
    return isfinite(dist_sq) && isfinite(r_sum) && dist_sq <= r_sum * r_sum ? 1 : 0;
}

/// @brief Compute the penetration vector to separate two overlapping spheres.
/// @param center_a Borrowed Vec3 center of sphere A.
/// @param radius_a Radius of sphere A; invalid values sanitize to zero.
/// @param center_b Borrowed Vec3 center of sphere B.
/// @param radius_b Radius of sphere B; invalid values sanitize to zero.
/// @return New vector that pushes A out of B, or zero when disjoint or invalid.
void *rt_sphere3d_penetration(void *center_a, double radius_a, void *center_b, double radius_b) {
    double ca[3], cb[3];
    if (!vec3_read_finite(center_a, ca) || !vec3_read_finite(center_b, cb))
        return rt_vec3_new(0, 0, 0);
    radius_a = raycast3d_sanitize_distance(radius_a);
    radius_b = raycast3d_sanitize_distance(radius_b);
    double dx = cb[0] - ca[0];
    double dy = cb[1] - ca[1];
    double dz = cb[2] - ca[2];
    double dist_sq = dx * dx + dy * dy + dz * dz;
    if (!isfinite(dist_sq))
        return rt_vec3_new(0, 0, 0);
    double dist = sqrt(dist_sq);
    double r_sum = radius_a + radius_b;
    if (r_sum <= 0.0 || dist >= r_sum)
        return rt_vec3_new(0, 0, 0);
    double depth = r_sum - dist;
    if (dist < 1e-12)
        return vec3_new_sanitized(0, raycast3d_sanitize_distance(depth), 0);
    double inv_dist = 1.0 / dist;
    return vec3_new_sanitized(
        -dx * inv_dist * depth, -dy * inv_dist * depth, -dz * inv_dist * depth);
}

/// @brief Find the closest point on an AABB surface to a given point.
/// @details Points inside the box project to the nearest face rather than being
///          returned unchanged; points outside clamp independently to the box.
/// @param aabb_min Borrowed Vec3 minimum box corner.
/// @param aabb_max Borrowed Vec3 maximum box corner.
/// @param point Borrowed Vec3 query point.
/// @return New sanitized Vec3 on the box surface, or zero for invalid input.
void *rt_aabb3d_closest_point(void *aabb_min, void *aabb_max, void *point) {
    double p[3], mn[3], mx[3];
    if (!vec3_read_finite(point, p) || !vec3_read_finite(aabb_min, mn) ||
        !vec3_read_finite(aabb_max, mx))
        return rt_vec3_new(0, 0, 0);
    aabb3d_canonicalize_raw(mn, mx);
    {
        double c[3];
        aabb3d_clamp_point_raw(mn, mx, p, c);
        if (p[0] >= mn[0] && p[0] <= mx[0] && p[1] >= mn[1] && p[1] <= mx[1] && p[2] >= mn[2] &&
            p[2] <= mx[2]) {
            double dx0 = fabs(p[0] - mn[0]), dx1 = fabs(mx[0] - p[0]);
            double dy0 = fabs(p[1] - mn[1]), dy1 = fabs(mx[1] - p[1]);
            double dz0 = fabs(p[2] - mn[2]), dz1 = fabs(mx[2] - p[2]);
            double best = dx0;
            c[0] = mn[0];
            c[1] = p[1];
            c[2] = p[2];
            if (dx1 < best) {
                best = dx1;
                c[0] = mx[0];
                c[1] = p[1];
                c[2] = p[2];
            }
            if (dy0 < best) {
                best = dy0;
                c[0] = p[0];
                c[1] = mn[1];
                c[2] = p[2];
            }
            if (dy1 < best) {
                best = dy1;
                c[0] = p[0];
                c[1] = mx[1];
                c[2] = p[2];
            }
            if (dz0 < best) {
                best = dz0;
                c[0] = p[0];
                c[1] = p[1];
                c[2] = mn[2];
            }
            if (dz1 < best) {
                c[0] = p[0];
                c[1] = p[1];
                c[2] = mx[2];
            }
        }
        return vec3_new_sanitized(c[0], c[1], c[2]);
    }
}

/// @brief Test whether an AABB and a sphere overlap.
/// @param aabb_min Borrowed Vec3 minimum box corner.
/// @param aabb_max Borrowed Vec3 maximum box corner.
/// @param center Borrowed Vec3 sphere center.
/// @param radius Non-negative sphere radius.
/// @return Nonzero when the closed shapes overlap or touch.
int8_t rt_aabb3d_sphere_overlaps(void *aabb_min, void *aabb_max, void *center, double radius) {
    double p[3], mn[3], mx[3];
    if (!vec3_read_finite(center, p) || !vec3_read_finite(aabb_min, mn) ||
        !vec3_read_finite(aabb_max, mx) || !isfinite(radius) || radius < 0.0)
        return 0;
    radius = raycast3d_sanitize_distance(radius);
    aabb3d_canonicalize_raw(mn, mx);
    {
        double c[3];
        aabb3d_clamp_point_raw(mn, mx, p, c);
        {
            double dx = p[0] - c[0];
            double dy = p[1] - c[1];
            double dz = p[2] - c[2];
            double dist_sq = dx * dx + dy * dy + dz * dz;
            return isfinite(dist_sq) && dist_sq <= radius * radius ? 1 : 0;
        }
    }
}

/// @brief Closest point on a finite line segment to a query point. Projects the point
/// onto the segment direction, clamps the parametric `t` to `[0, 1]` (so the result
/// stays on the segment, never on its infinite extension), and reconstructs the world
/// position. Degenerate zero-length segments collapse to endpoint A. Used by capsule
/// overlap tests and AI path-following.
/// @param seg_a Borrowed Vec3 first segment endpoint.
/// @param seg_b Borrowed Vec3 second segment endpoint.
/// @param point Borrowed Vec3 query point.
/// @return New sanitized Vec3 on the finite segment, or zero for invalid input.
void *rt_segment3d_closest_point(void *seg_a, void *seg_b, void *point) {
    double a[3], b[3], p[3];
    double closest[3];
    if (!vec3_read_finite(seg_a, a) || !vec3_read_finite(seg_b, b) || !vec3_read_finite(point, p))
        return rt_vec3_new(0, 0, 0);
    segment3d_closest_point_raw(a, b, p, closest);
    return vec3_new_sanitized(closest[0], closest[1], closest[2]);
}

/// @brief Capsule-vs-sphere overlap. Reduces to a point-segment distance check —
/// the capsule's surface is everywhere `cap_radius` from its core segment, so the
/// sphere intersects the capsule when the sphere centre is within `cap_radius +
/// sphere_radius` of the closest point on the segment. Squared comparison avoids the
/// sqrt.
/// @param cap_a Borrowed Vec3 first capsule-axis endpoint.
/// @param cap_b Borrowed Vec3 second capsule-axis endpoint.
/// @param cap_radius Capsule radius; invalid values sanitize to zero.
/// @param sphere_center Borrowed Vec3 sphere center.
/// @param sphere_radius Sphere radius; invalid values sanitize to zero.
/// @return Nonzero when the closed capsule and sphere overlap or touch.
int8_t rt_capsule3d_sphere_overlaps(
    void *cap_a, void *cap_b, double cap_radius, void *sphere_center, double sphere_radius) {
    double a[3], b[3], c[3];
    double closest[3];
    if (!vec3_read_finite(cap_a, a) || !vec3_read_finite(cap_b, b) ||
        !vec3_read_finite(sphere_center, c))
        return 0;
    cap_radius = raycast3d_sanitize_distance(cap_radius);
    sphere_radius = raycast3d_sanitize_distance(sphere_radius);
    segment3d_closest_point_raw(a, b, c, closest);
    double dx = c[0] - closest[0];
    double dy = c[1] - closest[1];
    double dz = c[2] - closest[2];
    double r_sum = cap_radius + sphere_radius;
    double dist_sq = dx * dx + dy * dy + dz * dz;
    return isfinite(dist_sq) && isfinite(r_sum) && dist_sq <= r_sum * r_sum ? 1 : 0;
}

/// @brief Exact capsule-vs-AABB overlap using segment-to-box squared distance.
/// @details Minimises the convex point-to-AABB distance function over the capsule's
/// core segment by splitting the segment at box slab boundaries and checking each
/// interval's quadratic minimum. The capsule overlaps when that exact segment-box
/// distance is within the capsule radius.
/// @param cap_a Borrowed Vec3 first capsule-axis endpoint.
/// @param cap_b Borrowed Vec3 second capsule-axis endpoint.
/// @param radius Capsule radius; invalid values sanitize to zero.
/// @param aabb_min Borrowed Vec3 minimum box corner.
/// @param aabb_max Borrowed Vec3 maximum box corner.
/// @return Nonzero when the closed capsule and box overlap or touch.
int8_t rt_capsule3d_aabb_overlaps(
    void *cap_a, void *cap_b, double radius, void *aabb_min, void *aabb_max) {
    double a[3], b[3], mn[3], mx[3];
    if (!vec3_read_finite(cap_a, a) || !vec3_read_finite(cap_b, b) ||
        !vec3_read_finite(aabb_min, mn) || !vec3_read_finite(aabb_max, mx))
        return 0;
    aabb3d_canonicalize_raw(mn, mx);
    radius = raycast3d_sanitize_distance(radius);
    double dist_sq = segment_aabb_distance_sq_raw(a, b, mn, mx);
    return isfinite(dist_sq) && dist_sq <= radius * radius ? 1 : 0;
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
