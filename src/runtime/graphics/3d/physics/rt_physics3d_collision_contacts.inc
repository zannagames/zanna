//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/physics/rt_physics3d_collision_contacts.inc  (#included from
// rt_physics3d_collision.c; not a standalone translation unit) Purpose: Physics3D contact
// generation, OBB SAT, and manifold helpers
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d_collision_contacts.inc
 * @brief Implements contact geometry, manifold expansion, pair identity, and collision filtering.
 *
 * This fragment provides primitive distance helpers, query and body-pair
 * filters, bounded manifold construction for capsules, boxes, OBBs, meshes,
 * and heightfields, frame-contact aggregation, hash-based contact lookup, and
 * representative support-point reconstruction.
 */

/// @brief Build a stack-allocated dummy sphere body for transient queries.
///
/// Used so that overlap / shapecast routines can reuse the body-vs-body
/// collision functions without needing to register a permanent body.
/// Only fields the collision routines read are populated.
/// @param out Caller-provided Body3D storage initialized as a transient sphere.
/// @param center Three-component world-space sphere center.
/// @param radius Sphere radius used by collision routines.
static void make_temp_sphere(rt_body3d *out, const double *center, double radius) {
    rt_body3d zero = {0};
    *out = zero;
    out->shape = PH3D_SHAPE_SPHERE;
    out->position[0] = center[0];
    out->position[1] = center[1];
    out->position[2] = center[2];
    quat_identity(out->orientation);
    vec3_set(out->scale, 1.0, 1.0, 1.0);
    out->radius = radius;
}

/// @brief Return the two world-space endpoints of a capsule's oriented axis segment.
///
/// Capsules are authored along local Y with the body position at the center.
/// The body orientation rotates that local axis before collision tests use it.
/// @param b Capsule Body3D whose center, height, radius, and orientation are read.
/// @param a Three-component output receiving the negative-axis endpoint.
/// @param c Three-component output receiving the positive-axis endpoint.
void capsule_axis_endpoints(const rt_body3d *b, double *a, double *c) {
    double half_axis = fmax(b->height * 0.5 - b->radius, 0.0);
    double local_axis[3] = {0.0, half_axis, 0.0};
    double axis[3];
    quat_rotate_vec3(b->orientation, local_axis, axis);
    vec3_set(a, b->position[0] - axis[0], b->position[1] - axis[1], b->position[2] - axis[2]);
    vec3_set(c, b->position[0] + axis[0], b->position[1] + axis[1], b->position[2] + axis[2]);
}

/// @brief Find the point on segment [a, b] closest to @p point.
/// @details Projects @p point onto the line through a and b, clamps the
///   parameter t to [0, 1] to stay within the segment, then evaluates
///   the position.  The degenerate case (a == b, denom ≈ 0) returns a.
/// @param a        Segment start endpoint (world space).
/// @param b        Segment end endpoint (world space).
/// @param point    Query point (world space).
/// @param closest  Output: closest point on the segment to @p point.
static void closest_point_on_segment(const double *a,
                                     const double *b,
                                     const double *point,
                                     double *closest) {
    double ab[3], ap[3];
    vec3_sub(b, a, ab);
    vec3_sub(point, a, ap);
    double denom = vec3_dot(ab, ab);
    double t = denom > 1e-18 ? vec3_dot(ap, ab) / denom : 0.0;
    t = clampd(t, 0.0, 1.0);
    closest[0] = a[0] + ab[0] * t;
    closest[1] = a[1] + ab[1] * t;
    closest[2] = a[2] + ab[2] * t;
}

/// @brief Find the pair of closest points between two line segments in 3D.
/// @details Implements the GDC/Ericson Real-Time Collision Detection segment–segment
///   closest-point algorithm.  Handles degenerate cases where either or both segments
///   collapse to a point (length ≤ eps).  The result is the world-space pair (c1, c2)
///   such that |c1 - c2| is minimized over the two segments.  Used in capsule–capsule
///   and capsule–box narrow-phase collision to compute the axis-to-axis gap.
/// @param p1  Start of segment 1.
/// @param q1  End of segment 1.
/// @param p2  Start of segment 2.
/// @param q2  End of segment 2.
/// @param c1  Output: closest point on segment 1.
/// @param c2  Output: closest point on segment 2.
static void closest_points_on_segments(const double *p1,
                                       const double *q1,
                                       const double *p2,
                                       const double *q2,
                                       double *c1,
                                       double *c2) {
    const double eps = 1e-18;
    double d1[3], d2[3], r[3];
    vec3_sub(q1, p1, d1);
    vec3_sub(q2, p2, d2);
    vec3_sub(p1, p2, r);
    double a = vec3_dot(d1, d1);
    double e = vec3_dot(d2, d2);
    double f = vec3_dot(d2, r);
    double s, t;

    if (a <= eps && e <= eps) {
        vec3_copy(c1, p1);
        vec3_copy(c2, p2);
        return;
    }
    if (a <= eps) {
        s = 0.0;
        t = clampd(f / e, 0.0, 1.0);
    } else {
        double c = vec3_dot(d1, r);
        if (e <= eps) {
            t = 0.0;
            s = clampd(-c / a, 0.0, 1.0);
        } else {
            double b = vec3_dot(d1, d2);
            double denom = a * e - b * b;
            if (denom != 0.0)
                s = clampd((b * f - c * e) / denom, 0.0, 1.0);
            else
                s = 0.0;
            t = (b * s + f) / e;
            if (t < 0.0) {
                t = 0.0;
                s = clampd(-c / a, 0.0, 1.0);
            } else if (t > 1.0) {
                t = 1.0;
                s = clampd((b - c) / a, 0.0, 1.0);
            }
        }
    }
    c1[0] = p1[0] + d1[0] * s;
    c1[1] = p1[1] + d1[1] * s;
    c1[2] = p1[2] + d1[2] * s;
    c2[0] = p2[0] + d2[0] * t;
    c2[1] = p2[1] + d2[1] * t;
    c2[2] = p2[2] + d2[2] * t;
}

/// @brief Compute the squared distance from @p point to the AABB defined by [mn, mx].
/// @details Projects the point onto the closest face/edge/corner of the box using
///   per-axis clamping, then returns the squared length of the gap vector.  Returns
///   0 for points inside the AABB.  Used by the segment-to-AABB search to evaluate
///   candidate t parameters quickly without a sqrt.
/// @param point  Query point (world space, double[3]).
/// @param mn     AABB minimum corner (world space, double[3]).
/// @param mx     AABB maximum corner (world space, double[3]).
/// @return       Squared Euclidean distance from @p point to the nearest AABB surface.
static double point_aabb_distance_sq(const double *point, const double *mn, const double *mx) {
    double q[3] = {clampd(point[0], mn[0], mx[0]),
                   clampd(point[1], mn[1], mx[1]),
                   clampd(point[2], mn[2], mx[2])};
    double delta[3];
    vec3_sub(point, q, delta);
    return vec3_len_sq(delta);
}

/// @brief Find the point on segment [a, c] that is closest to the AABB [mn, mx].
/// @details Uses a multi-probe sampling strategy: evaluates point_aabb_distance_sq
///   at t = 0, 1, and at each of the six per-axis candidate t values derived from
///   the AABB face planes (where the segment crosses each slab boundary).  The
///   sample with the smallest squared distance wins.  This avoids a full analytic
///   segment–AABB solver while remaining robust for the capsule–box narrow phase.
/// @param a           Segment start endpoint (world space, double[3]).
/// @param c           Segment end endpoint (world space, double[3]).
/// @param mn          AABB minimum corner (world space, double[3]).
/// @param mx          AABB maximum corner (world space, double[3]).
/// @param closest_axis  Output: best-candidate point on the segment (world space, double[3]).
static void closest_point_segment_to_aabb(
    const double *a, const double *c, const double *mn, const double *mx, double *closest_axis) {
    double d[3];
    double best_t = 0.0;
    double best_dist = 1e300;
    vec3_sub(c, a, d);

#define PH3D_EVAL_SEG_AABB_T(t_expr)                                                               \
    do {                                                                                           \
        double eval_t = clampd((t_expr), 0.0, 1.0);                                                \
        double p_eval[3] = {a[0] + d[0] * eval_t, a[1] + d[1] * eval_t, a[2] + d[2] * eval_t};     \
        double dist_eval = point_aabb_distance_sq(p_eval, mn, mx);                                 \
        if (dist_eval < best_dist) {                                                               \
            best_dist = dist_eval;                                                                 \
            best_t = eval_t;                                                                       \
        }                                                                                          \
    } while (0)

    PH3D_EVAL_SEG_AABB_T(0.0);
    PH3D_EVAL_SEG_AABB_T(1.0);
    {
        double center[3] = {
            (mn[0] + mx[0]) * 0.5,
            (mn[1] + mx[1]) * 0.5,
            (mn[2] + mx[2]) * 0.5,
        };
        double ac[3];
        vec3_sub(center, a, ac);
        double len_sq = vec3_len_sq(d);
        if (len_sq > 1e-18)
            PH3D_EVAL_SEG_AABB_T(vec3_dot(ac, d) / len_sq);
    }
    for (int axis = 0; axis < 3; axis++) {
        if (fabs(d[axis]) > 1e-18) {
            PH3D_EVAL_SEG_AABB_T((mn[axis] - a[axis]) / d[axis]);
            PH3D_EVAL_SEG_AABB_T((mx[axis] - a[axis]) / d[axis]);
        }
    }
    {
        double lo = 0.0;
        double hi = 1.0;
        for (int iter = 0; iter < 24; iter++) {
            double m1 = lo + (hi - lo) / 3.0;
            double m2 = hi - (hi - lo) / 3.0;
            double p1[3] = {a[0] + d[0] * m1, a[1] + d[1] * m1, a[2] + d[2] * m1};
            double p2[3] = {a[0] + d[0] * m2, a[1] + d[1] * m2, a[2] + d[2] * m2};
            if (point_aabb_distance_sq(p1, mn, mx) < point_aabb_distance_sq(p2, mn, mx))
                hi = m2;
            else
                lo = m1;
        }
        PH3D_EVAL_SEG_AABB_T((lo + hi) * 0.5);
    }

#undef PH3D_EVAL_SEG_AABB_T

    closest_axis[0] = a[0] + d[0] * best_t;
    closest_axis[1] = a[1] + d[1] * best_t;
    closest_axis[2] = a[2] + d[2] * best_t;
}

/// @brief Project a point onto the capsule's axis segment.
///
/// Used as the first step in capsule-vs-anything distance computations.
/// @param cap Capsule Body3D defining the oriented axis segment.
/// @param point Three-component world-space query point.
/// @param closest Three-component output receiving the closest axis point.
static void closest_point_capsule_axis_to_point(const rt_body3d *cap,
                                                const double *point,
                                                double *closest) {
    double a[3], c[3];
    capsule_axis_endpoints(cap, a, c);
    closest_point_on_segment(a, c, point, closest);
}

/// @brief Find the closest point pair on two capsule axes.
///
/// Handles arbitrary capsule orientations by solving segment-vs-segment.
/// @param a First capsule Body3D.
/// @param b Second capsule Body3D.
/// @param closest_a Three-component output on @p a's axis.
/// @param closest_b Three-component output on @p b's axis.
static void closest_points_capsule_axes(const rt_body3d *a,
                                        const rt_body3d *b,
                                        double *closest_a,
                                        double *closest_b) {
    double aa[3], ac[3], ba[3], bc[3];
    capsule_axis_endpoints(a, aa, ac);
    capsule_axis_endpoints(b, ba, bc);
    closest_points_on_segments(aa, ac, ba, bc, closest_a, closest_b);
}

/// @brief Find the point on a capsule's axis closest to a box.
///
/// Used as the first step of capsule-vs-AABB collision.
/// @param cap Capsule Body3D defining the source segment.
/// @param box Box Body3D whose world AABB is queried.
/// @param closest_axis Three-component output receiving the closest segment point.
static void closest_point_capsule_axis_to_aabb(const rt_body3d *cap,
                                               const rt_body3d *box,
                                               double *closest_axis) {
    double mn[3], mx[3], a[3], c[3];
    body_aabb(box, mn, mx);
    capsule_axis_endpoints(cap, a, c);
    closest_point_segment_to_aabb(a, c, mn, mx, closest_axis);
}

/// @brief Layer/mask filter for one-sided queries (raycast, overlap).
///
/// Bidirectional layer/mask filtering applies for body-vs-body collision
/// (`bodies_can_collide`); queries are unidirectional, so we just check
/// the body's layer against the query's mask. A zero mask matches nothing,
/// which keeps `LayerMask.None()` consistent across high-level queries.
/// @param body Candidate Body3D whose collision layer is tested.
/// @param mask Query collision-layer mask.
/// @return One when the body's layer intersects @p mask, otherwise zero.
int query_mask_matches_body(const rt_body3d *body, int64_t mask) {
    if (!body)
        return 0;
    if (mask == 0)
        return 0;
    return (body->collision_layer & mask) != 0;
}

/// @brief Raw AABB-vs-AABB overlap test (no shape interpretation).
///
/// SAT on the axis-aligned axes. Returns true if neither axis fully
/// separates the two boxes. This is the broad-phase primitive — the
/// narrow-phase shape tests run only on pairs that pass this check.
/// @param amn First AABB minimum corner.
/// @param amx First AABB maximum corner.
/// @param bmn Second AABB minimum corner.
/// @param bmx Second AABB maximum corner.
/// @return One when the closed AABBs overlap, otherwise zero.
int aabb_overlap_raw(const double *amn, const double *amx, const double *bmn, const double *bmx) {
    return !(amn[0] > bmx[0] || amx[0] < bmn[0] || amn[1] > bmx[1] || amx[1] < bmn[1] ||
             amn[2] > bmx[2] || amx[2] < bmn[2]);
}

/// @brief Compute the AABB swept by translating `[start_min, start_max]` by `delta`.
///
/// Used by CCD to make a single AABB that bounds an entire integration
/// step's motion. If this swept box doesn't overlap a candidate body's
/// AABB, the bodies cannot collide during the step.
/// @param start_min Initial AABB minimum corner.
/// @param start_max Initial AABB maximum corner.
/// @param delta Three-component translation applied during the step.
/// @param swept_min Output receiving the union AABB minimum corner.
/// @param swept_max Output receiving the union AABB maximum corner.
void swept_aabb_from_points(const double *start_min,
                            const double *start_max,
                            const double *delta,
                            double *swept_min,
                            double *swept_max) {
    double end_min[3] = {start_min[0] + delta[0], start_min[1] + delta[1], start_min[2] + delta[2]};
    double end_max[3] = {start_max[0] + delta[0], start_max[1] + delta[1], start_max[2] + delta[2]};
    swept_min[0] = start_min[0] < end_min[0] ? start_min[0] : end_min[0];
    swept_min[1] = start_min[1] < end_min[1] ? start_min[1] : end_min[1];
    swept_min[2] = start_min[2] < end_min[2] ? start_min[2] : end_min[2];
    swept_max[0] = start_max[0] > end_max[0] ? start_max[0] : end_max[0];
    swept_max[1] = start_max[1] > end_max[1] ? start_max[1] : end_max[1];
    swept_max[2] = start_max[2] > end_max[2] ? start_max[2] : end_max[2];
}

/// @brief Bytewise contact copy. Used to snapshot frame-N contacts for
///        the frame-N+1 enter/stay/exit event diff.
/// @param dst Destination contact record.
/// @param src Source contact record.
void contact_snapshot_copy(rt_contact3d *dst, const rt_contact3d *src) {
    if (!dst || !src)
        return;
    memcpy(dst, src, sizeof(*dst));
}

/// @brief Whether the body's orientation quaternion is (within 1e-9) the identity.
/// @details Lets axis-aligned collision paths skip rotation math for un-rotated bodies.
/// @param body Body3D whose orientation is tested.
/// @return One when the quaternion is approximately identity, otherwise zero.
static int body3d_orientation_is_identity(const rt_body3d *body) {
    if (!body)
        return 0;
    return fabs(body->orientation[0]) < 1e-9 && fabs(body->orientation[1]) < 1e-9 &&
           fabs(body->orientation[2]) < 1e-9 && fabs(body->orientation[3] - 1.0) < 1e-9;
}

/// @brief Write a manifold point (position, normal, separation) at @p index (bounds-checked).
/// @param contact Contact record whose inline manifold is modified.
/// @param index Zero-based manifold slot.
/// @param point Three-component world-space contact point.
/// @param normal Three-component A-to-B contact normal.
/// @param separation Signed contact separation.
static void contact3d_set_manifold_point(rt_contact3d *contact,
                                         int32_t index,
                                         const double *point,
                                         const double *normal,
                                         double separation) {
    if (!contact || !point || !normal || index < 0 || index >= PH3D_MAX_MANIFOLD_POINTS)
        return;
    vec3_copy(contact->points[index], point);
    vec3_copy(contact->normals[index], normal);
    contact->separations[index] = separation;
}

/// @brief Whether @p point already appears in the manifold (within 1e-6 distance) — a dedup guard.
/// @param contact Contact record whose current manifold is searched.
/// @param point Three-component candidate world-space point.
/// @return One when the point is invalid or already represented, otherwise zero.
static int contact3d_point_exists(const rt_contact3d *contact, const double *point) {
    if (!contact || !point)
        return 1;
    for (int32_t i = 0; i < contact->contact_count; i++) {
        double dx = contact->points[i][0] - point[0];
        double dy = contact->points[i][1] - point[1];
        double dz = contact->points[i][2] - point[2];
        if (dx * dx + dy * dy + dz * dz < 1e-12)
            return 1;
    }
    return 0;
}

/// @brief Reset the contact manifold to a single point (position, normal, separation).
/// @param contact Contact record to initialize.
/// @param point Three-component world-space contact point.
/// @param normal Three-component A-to-B contact normal.
/// @param separation Signed contact separation.
void contact3d_init_single_point(rt_contact3d *contact,
                                 const double *point,
                                 const double *normal,
                                 double separation) {
    if (!contact || !point || !normal)
        return;
    contact->contact_count = 1;
    contact3d_set_manifold_point(contact, 0, point, normal, separation);
}

/// @brief Add a manifold point if it is finite, non-duplicate, and the manifold isn't full.
/// @param contact Contact record whose bounded manifold may grow.
/// @param point Three-component world-space candidate point.
/// @param normal Three-component A-to-B contact normal.
/// @param separation Signed contact separation.
void contact3d_try_add_manifold_point(rt_contact3d *contact,
                                      const double *point,
                                      const double *normal,
                                      double separation) {
    if (!contact || !point || !normal || !ph3d_vec3_all_finite(point) ||
        !ph3d_vec3_all_finite(normal))
        return;
    if (contact->contact_count >= PH3D_MAX_MANIFOLD_POINTS)
        return;
    if (contact3d_point_exists(contact, point))
        return;
    contact3d_set_manifold_point(contact, contact->contact_count++, point, normal, separation);
}

/// @brief Promote a single-point lengthwise capsule contact to a two-point manifold.
/// @details A capsule resting on its side touches along a line, but the sphere-sample
///          narrow phase reports only the deepest sample, so the solver receives one
///          point and the capsule rocks/jitters instead of resting flat. Projecting
///          both axis endpoints onto the contact plane through the reported point
///          gives the solver a stable two-point patch. Fires only when the capsule
///          axis is roughly perpendicular to the contact normal — end-standing
///          capsules keep their correct single point.
/// @param contact Single-point contact record to expand when appropriate.
/// @param a First Body3D in the contact pair.
/// @param b Second Body3D in the contact pair.
void contact3d_expand_capsule_manifold(rt_contact3d *contact,
                                       const rt_body3d *a,
                                       const rt_body3d *b) {
    const rt_body3d *cap = NULL;
    double e0[3];
    double e1[3];
    double axis[3];
    double axis_len;
    double align;

    if (!contact || !a || !b || contact->contact_count != 1)
        return;
    if (a->shape == PH3D_SHAPE_CAPSULE && b->shape != PH3D_SHAPE_CAPSULE)
        cap = a;
    else if (b->shape == PH3D_SHAPE_CAPSULE && a->shape != PH3D_SHAPE_CAPSULE)
        cap = b;
    if (!cap)
        return;
    capsule_axis_endpoints(cap, e0, e1);
    vec3_sub(e1, e0, axis);
    axis_len = vec3_len(axis);
    if (!isfinite(axis_len) || axis_len < 1e-6)
        return;
    align = (axis[0] * contact->normal[0] + axis[1] * contact->normal[1] +
             axis[2] * contact->normal[2]) /
            axis_len;
    if (!isfinite(align) || fabs(align) > 0.3)
        return; /* end-on contact: the single point is the right manifold */

    const double *ends[2] = {e0, e1};
    for (int i = 0; i < 2; i++) {
        double to_plane = (contact->point[0] - ends[i][0]) * contact->normal[0] +
                          (contact->point[1] - ends[i][1]) * contact->normal[1] +
                          (contact->point[2] - ends[i][2]) * contact->normal[2];
        double projected[3] = {ends[i][0] + contact->normal[0] * to_plane,
                               ends[i][1] + contact->normal[1] * to_plane,
                               ends[i][2] + contact->normal[2] * to_plane};
        contact3d_try_add_manifold_point(contact, projected, contact->normal, contact->separation);
    }
}

/// @brief Promote a single-point box/box contact to a multi-point manifold over the AABB overlap.
/// @details Samples the corners of the two bodies' overlapping AABB region as extra contact points,
///          giving the solver a stable contact patch (resting boxes don't wobble on one point).
/// @param contact Contact record to expand.
/// @param a First axis-aligned box Body3D.
/// @param b Second axis-aligned box Body3D.
void contact3d_expand_aabb_manifold(rt_contact3d *contact, const rt_body3d *a, const rt_body3d *b) {
    double a_min[3];
    double a_max[3];
    double b_min[3];
    double b_max[3];
    double overlap_min[3];
    double overlap_max[3];
    int axis = 0;
    int u;
    int v;
    if (!contact || !a || !b)
        return;
    if (a->shape != PH3D_SHAPE_AABB || b->shape != PH3D_SHAPE_AABB)
        return;
    if (!body3d_orientation_is_identity(a) || !body3d_orientation_is_identity(b))
        return;
    if (fabs(contact->normal[1]) > fabs(contact->normal[axis]))
        axis = 1;
    if (fabs(contact->normal[2]) > fabs(contact->normal[axis]))
        axis = 2;
    u = (axis + 1) % 3;
    v = (axis + 2) % 3;
    for (int i = 0; i < 3; i++) {
        a_min[i] = a->position[i] - a->half_extents[i];
        a_max[i] = a->position[i] + a->half_extents[i];
        b_min[i] = b->position[i] - b->half_extents[i];
        b_max[i] = b->position[i] + b->half_extents[i];
        overlap_min[i] = a_min[i] > b_min[i] ? a_min[i] : b_min[i];
        overlap_max[i] = a_max[i] < b_max[i] ? a_max[i] : b_max[i];
        if (!isfinite(overlap_min[i]) || !isfinite(overlap_max[i]) ||
            overlap_min[i] > overlap_max[i])
            return;
    }

    double p[3] = {contact->point[0], contact->point[1], contact->point[2]};
    p[axis] = contact->point[axis];
    /* Keep the center support point, then add three face points whose centroid
     * is the center so resting AABB stacks do not accumulate contact torque. */
    p[u] = overlap_min[u];
    p[v] = contact->point[v];
    contact3d_try_add_manifold_point(contact, p, contact->normal, contact->separation);
    p[u] = overlap_max[u];
    p[v] = overlap_max[v];
    contact3d_try_add_manifold_point(contact, p, contact->normal, contact->separation);
    p[u] = contact->point[u];
    p[v] = overlap_min[v];
    contact3d_try_add_manifold_point(contact, p, contact->normal, contact->separation);
}

#define PH3D_OBB_CLIP_MAX_POINTS 16

typedef struct {
    double center[3];
    double axes[3][3];
    double half_extents[3];
} ph3d_obb_box;

typedef struct {
    double point[3];
    double separation;
} ph3d_obb_contact_candidate;

/// @brief Build an oriented-bounding-box description (center, world axes, scaled half-extents)
///        from a box collider and its world pose.
/// @param collider Box Collider3D handle to describe.
/// @param pose World pose applied to the collider.
/// @param out Output receiving the complete oriented box.
/// @return 1 if @p collider is a box and the resulting center/extents are finite; 0 otherwise.
static int ph3d_make_obb_box(void *collider, const rt_collider_pose *pose, ph3d_obb_box *out) {
    if (!collider || !pose || !out || rt_collider3d_get_type(collider) != RT_COLLIDER3D_TYPE_BOX)
        return 0;
    vec3_copy(out->center, pose->position);
    pose_rotation_axes(pose, out->axes);
    box_scaled_half_extents(collider, pose, out->half_extents);
    return ph3d_vec3_all_finite(out->center) && ph3d_vec3_all_finite(out->half_extents);
}

/// @brief Pick the box local axis whose face normal is most aligned with @p target_normal.
/// @details Scans the three local axes and selects the one with the largest |dot| against
///          @p target_normal; the sign chooses which of the two opposing faces points toward it.
/// @param box Oriented box whose face axes are inspected.
/// @param target_normal Three-component world direction used for alignment.
/// @param axis_out Receives the chosen axis index (0..2).
/// @param sign_out Receives +1/-1 selecting the positive/negative face along that axis.
/// @param alignment_out Receives the alignment magnitude (|dot|, in [0,1]).
/// @return 1 if a positively-aligned face was found; 0 if @p box or @p target_normal is NULL.
static int ph3d_obb_best_face_axis(const ph3d_obb_box *box,
                                   const double *target_normal,
                                   int *axis_out,
                                   double *sign_out,
                                   double *alignment_out) {
    double best_alignment = -1.0;
    int best_axis = 0;
    double best_sign = 1.0;
    if (!box || !target_normal)
        return 0;
    for (int axis = 0; axis < 3; ++axis) {
        double d = vec3_dot(target_normal, box->axes[axis]);
        double a = fabs(d);
        if (a > best_alignment) {
            best_alignment = a;
            best_axis = axis;
            best_sign = d >= 0.0 ? 1.0 : -1.0;
        }
    }
    if (axis_out)
        *axis_out = best_axis;
    if (sign_out)
        *sign_out = best_sign;
    if (alignment_out)
        *alignment_out = best_alignment;
    return best_alignment > 0.0;
}

/// @brief Compute the four world-space corner vertices of one OBB face.
/// @param box Oriented box defining the face.
/// @param face_axis Axis (0..2) whose face is selected.
/// @param face_sign +1/-1 picking the positive/negative face along @p face_axis.
/// @param vertices Output filled with the face's four corners (wound around the face).
static void ph3d_obb_face_vertices(const ph3d_obb_box *box,
                                   int face_axis,
                                   double face_sign,
                                   double vertices[4][3]) {
    int u = (face_axis + 1) % 3;
    int v = (face_axis + 2) % 3;
    const double signs[4][2] = {{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}};
    double face_center[3];
    for (int i = 0; i < 3; ++i)
        face_center[i] =
            box->center[i] + box->axes[face_axis][i] * box->half_extents[face_axis] * face_sign;
    for (int corner = 0; corner < 4; ++corner) {
        for (int i = 0; i < 3; ++i) {
            vertices[corner][i] = face_center[i] +
                                  box->axes[u][i] * box->half_extents[u] * signs[corner][0] +
                                  box->axes[v][i] * box->half_extents[v] * signs[corner][1];
        }
    }
}

/// @brief Sutherland-Hodgman clip a convex polygon against one OBB side half-space.
/// @details Keeps the part of the polygon satisfying `dot(point - origin, axis) <= limit`,
///          inserting intersection points wherever an edge crosses the plane. Output is capped
///          at PH3D_OBB_CLIP_MAX_POINTS.
/// @param in_points Input polygon vertices.
/// @param in_count Number of valid input vertices.
/// @param origin Point defining the clipping frame.
/// @param axis Outward half-space axis.
/// @param limit Maximum accepted projection from @p origin along @p axis.
/// @param out_points Output buffer receiving the clipped polygon.
/// @return Number of vertices written to @p out_points.
static int ph3d_clip_poly_to_extent(const double in_points[PH3D_OBB_CLIP_MAX_POINTS][3],
                                    int in_count,
                                    const double *origin,
                                    const double *axis,
                                    double limit,
                                    double out_points[PH3D_OBB_CLIP_MAX_POINTS][3]) {
    int out_count = 0;
    const double eps = 1e-8;
    if (!origin || !axis || in_count <= 0)
        return 0;
    for (int i = 0; i < in_count; ++i) {
        const double *s = in_points[i];
        const double *e = in_points[(i + 1) % in_count];
        double so[3];
        double eo[3];
        double edge[3];
        double ds;
        double de;
        int s_inside;
        int e_inside;
        vec3_sub(s, origin, so);
        vec3_sub(e, origin, eo);
        ds = vec3_dot(so, axis) - limit;
        de = vec3_dot(eo, axis) - limit;
        s_inside = ds <= eps;
        e_inside = de <= eps;
        vec3_sub(e, s, edge);
        if (s_inside && e_inside) {
            if (out_count < PH3D_OBB_CLIP_MAX_POINTS)
                vec3_copy(out_points[out_count++], e);
        } else if (s_inside && !e_inside) {
            double denom = ds - de;
            double t = fabs(denom) > 1e-12 ? ds / denom : 0.0;
            double p[3] = {s[0] + edge[0] * t, s[1] + edge[1] * t, s[2] + edge[2] * t};
            if (out_count < PH3D_OBB_CLIP_MAX_POINTS)
                vec3_copy(out_points[out_count++], p);
        } else if (!s_inside && e_inside) {
            double denom = ds - de;
            double t = fabs(denom) > 1e-12 ? ds / denom : 0.0;
            double p[3] = {s[0] + edge[0] * t, s[1] + edge[1] * t, s[2] + edge[2] * t};
            if (out_count < PH3D_OBB_CLIP_MAX_POINTS)
                vec3_copy(out_points[out_count++], p);
            if (out_count < PH3D_OBB_CLIP_MAX_POINTS)
                vec3_copy(out_points[out_count++], e);
        }
    }
    return out_count;
}

/// @brief Return 1 if @p candidate already appears in the first @p count entries of @p indices.
/// @param indices Candidate-index array to search.
/// @param count Number of valid entries in @p indices.
/// @param candidate Candidate index to find.
/// @return One when the candidate is already selected, otherwise zero.
static int ph3d_obb_candidate_exists(const int *indices, int count, int candidate) {
    for (int i = 0; i < count; ++i) {
        if (indices[i] == candidate)
            return 1;
    }
    return 0;
}

/// @brief Select the candidate contact point that extremizes one reference-face quadrant.
/// @details Projects each candidate onto the reference face's two tangent axes and scores it per
///          @p quadrant (each of the four quadrants favors a different (±u, ±v) corner), appending
///          the winning, not-yet-chosen index to @p indices. Used to reduce an over-full candidate
///          set to a well-spread PH3D_MAX_MANIFOLD_POINTS manifold.
/// @param candidates Contact candidates to score.
/// @param candidate_count Number of valid candidate records.
/// @param ref_box Reference OBB providing tangent axes.
/// @param ref_axis Reference face-normal axis index.
/// @param ref_face_center Three-component reference face center.
/// @param quadrant Tangent-space quadrant to extremize.
/// @param indices Output array of selected candidate indices.
/// @param index_count In/out count of selected indices.
static void ph3d_obb_select_candidate(const ph3d_obb_contact_candidate *candidates,
                                      int candidate_count,
                                      const ph3d_obb_box *ref_box,
                                      int ref_axis,
                                      const double *ref_face_center,
                                      int quadrant,
                                      int *indices,
                                      int *index_count) {
    int u = (ref_axis + 1) % 3;
    int v = (ref_axis + 2) % 3;
    double best_score = -DBL_MAX;
    int best_index = -1;
    for (int i = 0; i < candidate_count; ++i) {
        double rel[3];
        double coord_u;
        double coord_v;
        double score;
        vec3_sub(candidates[i].point, ref_face_center, rel);
        coord_u = vec3_dot(rel, ref_box->axes[u]);
        coord_v = vec3_dot(rel, ref_box->axes[v]);
        switch (quadrant) {
            case 0:
                score = -coord_u - coord_v;
                break;
            case 1:
                score = coord_u - coord_v;
                break;
            case 2:
                score = coord_u + coord_v;
                break;
            default:
                score = -coord_u + coord_v;
                break;
        }
        if (score > best_score) {
            best_score = score;
            best_index = i;
        }
    }
    if (best_index >= 0 && *index_count < PH3D_MAX_MANIFOLD_POINTS &&
        !ph3d_obb_candidate_exists(indices, *index_count, best_index)) {
        indices[(*index_count)++] = best_index;
    }
}

/// @brief Expand a single-point OBB/OBB contact into a stable multi-point manifold via face
/// clipping.
/// @details Reconstructs both boxes, picks the most-aligned face as the reference and the opposing
///          box's most anti-aligned face as the incident polygon, then clips the incident face
///          against the reference face's four side planes (ph3d_clip_poly_to_extent). Surviving
///          points within tolerance of the reference plane become contact candidates; when more
///          than PH3D_MAX_MANIFOLD_POINTS survive they are reduced to a spread set via
///          ph3d_obb_select_candidate. On too-poor face alignment or fewer than two surviving
///          points it restores the saved single-point @p contact and reports failure.
/// @param contact Single-point contact record to expand.
/// @param a_leaf First box leaf Collider3D.
/// @param a_pose World pose of @p a_leaf.
/// @param b_leaf Second box leaf Collider3D.
/// @param b_pose World pose of @p b_leaf.
/// @return 1 if a multi-point manifold was produced; 0 to keep the original single-point contact.
int contact3d_expand_obb_manifold(rt_contact3d *contact,
                                  void *a_leaf,
                                  const rt_collider_pose *a_pose,
                                  void *b_leaf,
                                  const rt_collider_pose *b_pose) {
    rt_contact3d fallback;
    ph3d_obb_box box_a;
    ph3d_obb_box box_b;
    const ph3d_obb_box *ref_box;
    const ph3d_obb_box *inc_box;
    double toward_b[3];
    double toward_a[3];
    double ref_normal[3];
    double final_normal[3];
    double ref_face_center[3];
    double incident_vertices[4][3];
    double poly_a[PH3D_OBB_CLIP_MAX_POINTS][3];
    double poly_b[PH3D_OBB_CLIP_MAX_POINTS][3];
    ph3d_obb_contact_candidate candidates[PH3D_OBB_CLIP_MAX_POINTS];
    int ref_axis = 0;
    int inc_axis = 0;
    int clipped_count = 4;
    int candidate_count = 0;
    double ref_sign = 1.0;
    double inc_sign = 1.0;
    double align_a = 0.0;
    double align_b = 0.0;
    const double min_face_alignment = 0.95;

    if (!contact || !ph3d_make_obb_box(a_leaf, a_pose, &box_a) ||
        !ph3d_make_obb_box(b_leaf, b_pose, &box_b))
        return 0;

    contact_snapshot_copy(&fallback, contact);
    vec3_copy(toward_b, contact->normal);
    if (vec3_normalize_in_place(toward_b) <= 1e-12)
        return 0;
    vec3_negate(toward_b, toward_a);

    ph3d_obb_best_face_axis(&box_a, toward_b, &ref_axis, &ref_sign, &align_a);
    ph3d_obb_best_face_axis(&box_b, toward_a, &inc_axis, &inc_sign, &align_b);
    if (align_a < min_face_alignment && align_b < min_face_alignment)
        return 0;

    if (align_b > align_a) {
        ref_box = &box_b;
        inc_box = &box_a;
        ph3d_obb_best_face_axis(ref_box, toward_a, &ref_axis, &ref_sign, NULL);
        vec3_negate(toward_b, ref_normal);
    } else {
        ref_box = &box_a;
        inc_box = &box_b;
        ph3d_obb_best_face_axis(ref_box, toward_b, &ref_axis, &ref_sign, NULL);
        vec3_copy(ref_normal, toward_b);
    }
    /* final_normal is the A->B contact direction (toward_b) for every manifold point,
     * independent of which box became the reference. ref_normal is the reference face's
     * outward normal (= +toward_b when A is the reference box, -toward_b when B is) and
     * is used only to measure `separation` and project points onto the mid-plane below.
     * In BOTH branches a penetrating incident point lies behind the reference face, so
     * `separation = dot(rel, ref_normal)` is negative for penetration in both cases —
     * the same sign convention the solver applies with final_normal. The (separation,
     * final_normal) pair is therefore consistent across the reference-flip; do not
     * "align" ref_normal and final_normal or the projection sign below would invert. */
    vec3_copy(final_normal, toward_b);

    for (int i = 0; i < 3; ++i)
        ref_face_center[i] = ref_box->center[i] + ref_box->axes[ref_axis][i] *
                                                      ref_box->half_extents[ref_axis] * ref_sign;

    {
        double best_inc_alignment = -1.0;
        for (int axis = 0; axis < 3; ++axis) {
            double d = vec3_dot(ref_normal, inc_box->axes[axis]);
            double a = fabs(d);
            if (a > best_inc_alignment) {
                best_inc_alignment = a;
                inc_axis = axis;
                inc_sign = d >= 0.0 ? -1.0 : 1.0;
            }
        }
    }

    ph3d_obb_face_vertices(inc_box, inc_axis, inc_sign, incident_vertices);
    for (int i = 0; i < 4; ++i)
        vec3_copy(poly_a[i], incident_vertices[i]);

    {
        int u = (ref_axis + 1) % 3;
        int v = (ref_axis + 2) % 3;
        double neg_u[3];
        double neg_v[3];
        vec3_negate(ref_box->axes[u], neg_u);
        vec3_negate(ref_box->axes[v], neg_v);
        clipped_count = ph3d_clip_poly_to_extent((const double (*)[3])poly_a,
                                                 clipped_count,
                                                 ref_face_center,
                                                 ref_box->axes[u],
                                                 ref_box->half_extents[u],
                                                 poly_b);
        clipped_count = ph3d_clip_poly_to_extent(
            (const double (*)[3])poly_b,
            clipped_count,
            ref_face_center,
            neg_u,
            ref_box->half_extents[u],
            poly_a);
        clipped_count = ph3d_clip_poly_to_extent((const double (*)[3])poly_a,
                                                 clipped_count,
                                                 ref_face_center,
                                                 ref_box->axes[v],
                                                 ref_box->half_extents[v],
                                                 poly_b);
        clipped_count = ph3d_clip_poly_to_extent(
            (const double (*)[3])poly_b,
            clipped_count,
            ref_face_center,
            neg_v,
            ref_box->half_extents[v],
            poly_a);
    }

    if (clipped_count <= 0)
        return 0;

    for (int i = 0; i < clipped_count && i < PH3D_OBB_CLIP_MAX_POINTS; ++i) {
        double rel[3];
        double separation;
        vec3_sub(poly_a[i], ref_face_center, rel);
        separation = vec3_dot(rel, ref_normal);
        if (separation <= 1e-6 && candidate_count < PH3D_OBB_CLIP_MAX_POINTS) {
            candidates[candidate_count].point[0] = poly_a[i][0] - 0.5 * separation * ref_normal[0];
            candidates[candidate_count].point[1] = poly_a[i][1] - 0.5 * separation * ref_normal[1];
            candidates[candidate_count].point[2] = poly_a[i][2] - 0.5 * separation * ref_normal[2];
            candidates[candidate_count].separation = separation;
            candidate_count++;
        }
    }
    if (candidate_count < 2)
        return 0;

    contact->contact_count = 0;
    if (candidate_count <= PH3D_MAX_MANIFOLD_POINTS) {
        for (int i = 0; i < candidate_count; ++i)
            contact3d_try_add_manifold_point(
                contact, candidates[i].point, final_normal, candidates[i].separation);
    } else {
        int indices[PH3D_MAX_MANIFOLD_POINTS];
        int index_count = 0;
        for (int quadrant = 0; quadrant < PH3D_MAX_MANIFOLD_POINTS; ++quadrant) {
            ph3d_obb_select_candidate(candidates,
                                      candidate_count,
                                      ref_box,
                                      ref_axis,
                                      ref_face_center,
                                      quadrant,
                                      indices,
                                      &index_count);
        }
        for (int i = 0; i < candidate_count && index_count < PH3D_MAX_MANIFOLD_POINTS; ++i) {
            if (!ph3d_obb_candidate_exists(indices, index_count, i))
                indices[index_count++] = i;
        }
        for (int i = 0; i < index_count; ++i) {
            int candidate_index = indices[i];
            contact3d_try_add_manifold_point(contact,
                                             candidates[candidate_index].point,
                                             final_normal,
                                             candidates[candidate_index].separation);
        }
    }

    if (contact->contact_count < 2) {
        contact_snapshot_copy(contact, &fallback);
        return 0;
    }
    vec3_copy(contact->point, contact->points[0]);
    vec3_copy(contact->normal, final_normal);
    contact->separation = contact->separations[0];
    return 1;
}

/// @brief Deterministic orthonormal tangent basis for a unit plane normal.
/// @details Mirrors the solver's ph3d_contact_tangents construction so spread
///   selection over a contact plane is stable frame to frame.
/// @param n Three-component unit plane normal.
/// @param t1 Three-component output receiving the first unit tangent.
/// @param t2 Three-component output receiving the second unit tangent.
static void contact3d_plane_tangents(const double *n, double *t1, double *t2) {
    if (fabs(n[0]) >= 0.57735) {
        vec3_set(t1, n[1], -n[0], 0.0);
    } else {
        vec3_set(t1, 0.0, n[2], -n[1]);
    }
    if (vec3_normalize_in_place(t1) < 1e-12)
        vec3_set(t1, 1.0, 0.0, 0.0);
    vec3_cross(n, t1, t2);
    if (vec3_normalize_in_place(t2) < 1e-12)
        vec3_set(t2, 0.0, 0.0, 1.0);
}

#define PH3D_SUPPORT_PLANE_MAX_CANDIDATES 64

typedef struct {
    double point[3];
    double distance; /* signed distance along the oriented contact normal */
} ph3d_support_plane_candidate;

/// @brief Append a support-plane candidate, keeping the deepest
///   PH3D_SUPPORT_PLANE_MAX_CANDIDATES when the buffer saturates.
/// @param candidates Fixed-capacity candidate buffer.
/// @param count In/out number of valid candidates.
/// @param point Three-component world-space support point.
/// @param distance Signed oriented distance from the contact plane.
static void ph3d_support_plane_push(ph3d_support_plane_candidate *candidates,
                                    int32_t *count,
                                    const double *point,
                                    double distance) {
    if (!candidates || !count || !isfinite(distance) || !ph3d_vec3_all_finite(point))
        return;
    if (*count < PH3D_SUPPORT_PLANE_MAX_CANDIDATES) {
        vec3_copy(candidates[*count].point, point);
        candidates[*count].distance = distance;
        (*count)++;
        return;
    }
    {
        int32_t worst = 0;
        for (int32_t i = 1; i < *count; ++i) {
            if (candidates[i].distance > candidates[worst].distance)
                worst = i;
        }
        if (distance < candidates[worst].distance) {
            vec3_copy(candidates[worst].point, point);
            candidates[worst].distance = distance;
        }
    }
}

/// @brief Expand a single-point contact against a mesh or heightfield surface into a
///   stable multi-point manifold from the OTHER shape's support vertices.
/// @details Mesh and heightfield narrow phases report only the deepest point, so a
///   crate or convex hull resting on terrain balances on ONE contact — it micro-rocks,
///   never settles, and slowly drifts. This pass treats the contact plane (through the
///   reported point, along the reported normal) as the local surface and collects the
///   solid side's support vertices (box corners, hull vertices) that lie within a
///   feature-scaled tolerance of the deepest one, spread-selecting up to
///   PH3D_MAX_MANIFOLD_POINTS well-separated points. Per-point separations grow with
///   the vertex's height above the deepest support so the solver loads them softly.
/// @param contact Single-point surface contact to expand.
/// @param a_leaf First leaf Collider3D.
/// @param a_pose World pose of @p a_leaf.
/// @param b_leaf Second leaf Collider3D.
/// @param b_pose World pose of @p b_leaf.
/// @return 1 when the manifold gained points, 0 to keep the single-point contact.
int contact3d_expand_surface_support_manifold(rt_contact3d *contact,
                                              void *a_leaf,
                                              const rt_collider_pose *a_pose,
                                              void *b_leaf,
                                              const rt_collider_pose *b_pose) {
    void *surface_leaf = NULL;
    void *solid_leaf = NULL;
    const rt_collider_pose *solid_pose = NULL;
    int solid_is_a = 0;
    int64_t a_type;
    int64_t b_type;
    double n[3];
    double solid_sign;
    ph3d_support_plane_candidate candidates[PH3D_SUPPORT_PLANE_MAX_CANDIDATES];
    int32_t candidate_count = 0;
    double char_size = 0.0;
    double d_min = DBL_MAX;
    double tol;

    if (!contact || contact->contact_count != 1 || !a_leaf || !b_leaf || !a_pose || !b_pose)
        return 0;
    a_type = rt_collider3d_get_type(a_leaf);
    b_type = rt_collider3d_get_type(b_leaf);
    if ((a_type == RT_COLLIDER3D_TYPE_MESH || a_type == RT_COLLIDER3D_TYPE_HEIGHTFIELD) &&
        (b_type == RT_COLLIDER3D_TYPE_BOX || b_type == RT_COLLIDER3D_TYPE_CONVEX_HULL)) {
        surface_leaf = a_leaf;
        solid_leaf = b_leaf;
        solid_pose = b_pose;
        solid_is_a = 0;
    } else if ((b_type == RT_COLLIDER3D_TYPE_MESH || b_type == RT_COLLIDER3D_TYPE_HEIGHTFIELD) &&
               (a_type == RT_COLLIDER3D_TYPE_BOX || a_type == RT_COLLIDER3D_TYPE_CONVEX_HULL)) {
        surface_leaf = b_leaf;
        solid_leaf = a_leaf;
        solid_pose = a_pose;
        solid_is_a = 1;
    } else {
        return 0;
    }
    (void)surface_leaf;

    vec3_copy(n, contact->normal);
    if (vec3_normalize_in_place(n) <= 1e-12)
        return 0;
    /* The contact normal points A->B. The solid's resting-face vertices sit on
     * its -n side when the solid is B (floor below pushes B up along +n) and on
     * its +n side when the solid is A; fold the sign so "smaller distance means
     * deeper support" in both arrangements. */
    solid_sign = solid_is_a ? -1.0 : 1.0;

    {
        double local_min[3];
        double local_max[3];
        rt_collider3d_get_local_bounds_raw(solid_leaf, local_min, local_max);
        char_size = DBL_MAX;
        for (int axis = 0; axis < 3; ++axis) {
            double extent = (local_max[axis] - local_min[axis]) *
                            pose_abs_scale_or_unit(solid_pose->scale[axis]) * 0.5;
            if (isfinite(extent) && extent < char_size)
                char_size = extent;
        }
        if (char_size == DBL_MAX || char_size <= 1e-9)
            return 0;
    }

    if (rt_collider3d_get_type(solid_leaf) == RT_COLLIDER3D_TYPE_BOX) {
        double he[3];
        rt_collider3d_get_box_half_extents_raw(solid_leaf, he);
        for (int i = 0; i < 8; ++i) {
            double local[3] = {
                (i & 1) ? he[0] : -he[0], (i & 2) ? he[1] : -he[1], (i & 4) ? he[2] : -he[2]};
            double world[3];
            double rel[3];
            transform_point_from_pose(solid_pose, local, world);
            vec3_sub(world, contact->point, rel);
            ph3d_support_plane_push(
                candidates, &candidate_count, world, solid_sign * vec3_dot(rel, n));
        }
    } else {
        rt_mesh3d *mesh = (rt_mesh3d *)rt_collider3d_get_mesh_raw(solid_leaf);
        uint32_t vertex_count = mesh ? rt_mesh3d_safe_vertex_count(mesh) : 0u;
        if (!mesh || vertex_count == 0)
            return 0;
        /* Two-pass support scan in LOCAL space so hulls of any vertex count
         * stay exact (the old first-4096 truncation could miss the true
         * resting face). world_rel·(sign·n) = local·m + C where
         * m = scale ⊙ (R⁻¹·(sign·n)) and C is constant per pose, so the
         * local dot ranks support depth identically. Pass 1 (3 mults/vert)
         * finds the minimum; pass 2 transforms only vertices within the
         * feature tolerance of it. */
        double signed_n[3] = {solid_sign * n[0], solid_sign * n[1], solid_sign * n[2]};
        double inv_rotation[4];
        double m_axis[3];
        double local_min_value = DBL_MAX;
        double support_tol;
        quat_conjugate(solid_pose->rotation, inv_rotation);
        quat_rotate_vec3(inv_rotation, signed_n, m_axis);
        m_axis[0] *= solid_pose->scale[0];
        m_axis[1] *= solid_pose->scale[1];
        m_axis[2] *= solid_pose->scale[2];
        for (uint32_t i = 0; i < vertex_count; ++i) {
            const float *vp = mesh->vertices[i].pos;
            double value =
                (double)vp[0] * m_axis[0] + (double)vp[1] * m_axis[1] + (double)vp[2] * m_axis[2];
            if (isfinite(value) && value < local_min_value)
                local_min_value = value;
        }
        if (local_min_value == DBL_MAX)
            return 0;
        /* Same tolerance the quadrant filter applies below (kept in sync). */
        support_tol = fmax(2.0 * fabs(contact->separation) + 0.005, 0.05 * char_size);
        if (support_tol > 0.45 * char_size)
            support_tol = 0.45 * char_size;
        for (uint32_t i = 0; i < vertex_count; ++i) {
            const float *vp = mesh->vertices[i].pos;
            double value =
                (double)vp[0] * m_axis[0] + (double)vp[1] * m_axis[1] + (double)vp[2] * m_axis[2];
            double local[3];
            double world[3];
            double rel[3];
            if (!isfinite(value) || value > local_min_value + support_tol)
                continue;
            local[0] = vp[0];
            local[1] = vp[1];
            local[2] = vp[2];
            transform_point_from_pose(solid_pose, local, world);
            vec3_sub(world, contact->point, rel);
            ph3d_support_plane_push(
                candidates, &candidate_count, world, solid_sign * vec3_dot(rel, n));
        }
    }
    if (candidate_count < 2)
        return 0;

    for (int32_t i = 0; i < candidate_count; ++i) {
        if (candidates[i].distance < d_min)
            d_min = candidates[i].distance;
    }
    if (!isfinite(d_min))
        return 0;
    /* Feature-scaled tolerance: wide enough to keep a mildly tilted resting face
     * (corners spread by extent*sin(tilt)), narrow enough to never reach the
     * opposite face (2x the minimum half-extent away). The 0.005 pad mirrors the
     * solver's PH3D_PENETRATION_SLOP. */
    tol = fmax(2.0 * fabs(contact->separation) + 0.005, 0.05 * char_size);
    if (tol > 0.45 * char_size)
        tol = 0.45 * char_size;

    {
        double t1[3];
        double t2[3];
        int32_t added = 0;
        contact3d_plane_tangents(n, t1, t2);
        for (int quadrant = 0; quadrant < 4 && contact->contact_count < PH3D_MAX_MANIFOLD_POINTS;
             ++quadrant) {
            double best_score = -DBL_MAX;
            int32_t best_index = -1;
            for (int32_t i = 0; i < candidate_count; ++i) {
                double rel[3];
                double cu;
                double cv;
                double score;
                if (candidates[i].distance > d_min + tol)
                    continue;
                vec3_sub(candidates[i].point, contact->point, rel);
                cu = vec3_dot(rel, t1);
                cv = vec3_dot(rel, t2);
                switch (quadrant) {
                    case 0:
                        score = -cu - cv;
                        break;
                    case 1:
                        score = cu - cv;
                        break;
                    case 2:
                        score = cu + cv;
                        break;
                    default:
                        score = -cu + cv;
                        break;
                }
                if (score > best_score) {
                    best_score = score;
                    best_index = i;
                }
            }
            if (best_index < 0)
                continue;
            {
                int32_t before = contact->contact_count;
                double separation = contact->separation + (candidates[best_index].distance - d_min);
                contact3d_try_add_manifold_point(
                    contact, candidates[best_index].point, contact->normal, separation);
                if (contact->contact_count > before)
                    added++;
            }
        }
        return added > 0 ? 1 : 0;
    }
}

/// @brief Identity test for contacts: same body pair AND same collider pair.
///
/// The collider check matters for compound bodies — same body pair but
/// different child colliders is a different contact.
/// @param a First contact record to compare.
/// @param b Second contact record to compare.
/// @return One when both records identify the same unordered body/leaf pair, otherwise zero.
int contact_pair_equals(const rt_contact3d *a, const rt_contact3d *b) {
    if (!a || !b)
        return 0;
    return (a->body_a == b->body_a && a->body_b == b->body_b && a->collider_a == b->collider_a &&
            a->collider_b == b->collider_b) ||
           (a->body_a == b->body_b && a->body_b == b->body_a && a->collider_a == b->collider_b &&
            a->collider_b == b->collider_a);
}

/// @brief Fold `value` into running hash `key` (golden-ratio mix, à la Boost hash_combine);
///   never returns 0 so the result can serve as a non-empty-slot marker.
/// @param key Running non-cryptographic hash state.
/// @param value Pointer-sized value to combine.
/// @return Mixed nonzero hash state.
static uintptr_t contact_pair_hash_mix(uintptr_t key, uintptr_t value) {
    key ^= value + (uintptr_t)0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);
    return key ? key : (uintptr_t)1u;
}

/// @brief Hash a contact's identity (both body and both collider pointers) into one key.
/// @param contact Contact record whose unordered pair identity is hashed.
/// @return Nonzero pointer-sized hash invariant to pair order.
static uintptr_t contact_pair_hash_value(const rt_contact3d *contact) {
    uintptr_t body_a = (uintptr_t)contact->body_a;
    uintptr_t body_b = (uintptr_t)contact->body_b;
    uintptr_t collider_a = (uintptr_t)contact->collider_a;
    uintptr_t collider_b = (uintptr_t)contact->collider_b;
    uintptr_t key;
    if (body_b < body_a || (body_b == body_a && collider_b < collider_a)) {
        uintptr_t tmp = body_a;
        body_a = body_b;
        body_b = tmp;
        tmp = collider_a;
        collider_a = collider_b;
        collider_b = tmp;
    }
    key = body_a;
    key = contact_pair_hash_mix(key, body_b);
    key = contact_pair_hash_mix(key, collider_a);
    key = contact_pair_hash_mix(key, collider_b);
    return key ? key : (uintptr_t)1u;
}

/// @brief Append a contact to the per-frame aggregate, replacing an existing
///        same-pair record so the latest substep's geometry is exposed.
/// @param w World3D whose frame-wide contact aggregate is updated.
/// @param contact Contact snapshot to append or replace.
/// @return One on success, otherwise zero after marking the step failed.
int world3d_append_frame_contact_unique(rt_world3d *w, const rt_contact3d *contact) {
    int32_t next_count;
    if (!w || !contact)
        return 1;
    for (int32_t i = 0; i < w->frame_contact_count; ++i) {
        if (contact_pair_equals(&w->frame_contacts[i], contact)) {
            contact_snapshot_copy(&w->frame_contacts[i], contact);
            return 1;
        }
    }
    if (!world3d_checked_increment(w->frame_contact_count, &next_count) ||
        !world3d_reserve_frame_contacts(w, next_count)) {
        world3d_step_fail(w, "Physics3D.World.Step: frame-contact allocation failed");
        return 0;
    }
    contact_snapshot_copy(&w->frame_contacts[w->frame_contact_count++], contact);
    return 1;
}

/// @brief Publish the frame-wide contact aggregate as the public contact list.
/// @param w World3D whose aggregate is copied into public contact storage.
/// @return One on success, otherwise zero after marking the step failed.
int world3d_publish_frame_contacts(rt_world3d *w) {
    if (!w)
        return 0;
    if (!world3d_reserve_contacts(w, w->frame_contact_count)) {
        world3d_step_fail(w, "Physics3D.World.Step: contact allocation failed");
        return 0;
    }
    w->contact_count = w->frame_contact_count;
    for (int32_t i = 0; i < w->frame_contact_count; ++i)
        contact_snapshot_copy(&w->contacts[i], &w->frame_contacts[i]);
    return 1;
}

/// @brief Choose a power-of-two table capacity above 2×`count` (kept sparse to bound
///   probe lengths); returns 0 on overflow or non-positive count.
/// @param count Positive number of contacts to store.
/// @param out_capacity Output receiving the selected power-of-two slot count.
/// @return One when a safe capacity was calculated, otherwise zero.
static int contact_pair_table_capacity(int32_t count, int32_t *out_capacity) {
    int32_t cap = 16;
    if (!out_capacity || count <= 0)
        return 0;
    if (count > INT32_MAX / 2)
        return 0;
    while (cap < count * 2) {
        if (cap > INT32_MAX / 2)
            return 0;
        cap *= 2;
    }
    if ((size_t)cap > SIZE_MAX / sizeof(contact_pair_hash_entry))
        return 0;
    *out_capacity = cap;
    return 1;
}

/// @brief Insert every contact into a zeroed linear-probe table of @p capacity slots.
/// @param table Caller-provided zeroed hash table.
/// @param capacity Power-of-two number of table slots.
/// @param contacts Contiguous contact records to index.
/// @param count Number of records in @p contacts.
static void contact_pair_table_fill(contact_pair_hash_entry *table,
                                    int32_t capacity,
                                    const rt_contact3d *contacts,
                                    int32_t count) {
    for (int32_t i = 0; i < count; ++i) {
        uintptr_t hash = contact_pair_hash_value(&contacts[i]);
        int32_t slot = (int32_t)(hash & (uintptr_t)(capacity - 1));
        for (int32_t probe = 0; probe < capacity; ++probe) {
            int32_t idx = (slot + probe) & (capacity - 1);
            if (!table[idx].contact) {
                table[idx].contact = &contacts[i];
                table[idx].hash = hash;
                break;
            }
        }
    }
}

/// @brief Build an open-addressing (linear-probe) hash set over `count` contacts so
///   membership can be tested in O(1); returns the table (caller frees) and its
///   capacity via @p out_capacity, or NULL on allocation failure.
/// @param contacts Contiguous contact records to index.
/// @param count Positive number of contact records.
/// @param out_capacity Output receiving the allocated table capacity.
/// @return Newly allocated hash table owned by the caller, or NULL on failure.
contact_pair_hash_entry *contact_pair_table_build(const rt_contact3d *contacts,
                                                  int32_t count,
                                                  int32_t *out_capacity) {
    int32_t capacity = 0;
    contact_pair_hash_entry *table;
    if (!contacts || count <= 0 || !out_capacity)
        return NULL;
    if (!contact_pair_table_capacity(count, &capacity))
        return NULL;
    table = (contact_pair_hash_entry *)calloc((size_t)capacity, sizeof(*table));
    if (!table)
        return NULL;
    contact_pair_table_fill(table, capacity, contacts, count);
    *out_capacity = capacity;
    return table;
}

/// @brief contact_pair_table_build, but backed by a world step-scratch slot.
/// @details Used on the per-substep warm-start path and the per-step event diff,
///   where a malloc/free per call would churn the allocator. The returned table
///   is only valid until the slot's next acquire; callers must NOT free it.
/// @param w World3D providing step-scratch storage.
/// @param scratch_slot Scratch arena slot to acquire.
/// @param contacts Contiguous contact records to index.
/// @param count Positive number of contact records.
/// @param out_capacity Output receiving the scratch table capacity.
/// @return Borrowed scratch-backed hash table, or NULL on failure.
contact_pair_hash_entry *contact_pair_table_build_scratch(rt_world3d *w,
                                                          int32_t scratch_slot,
                                                          const rt_contact3d *contacts,
                                                          int32_t count,
                                                          int32_t *out_capacity) {
    int32_t capacity = 0;
    contact_pair_hash_entry *table;
    if (!w || !contacts || count <= 0 || !out_capacity)
        return NULL;
    if (!contact_pair_table_capacity(count, &capacity))
        return NULL;
    table = (contact_pair_hash_entry *)world3d_step_scratch_acquire(
        w, scratch_slot, (size_t)capacity * sizeof(*table), 1);
    if (!table)
        return NULL;
    contact_pair_table_fill(table, capacity, contacts, count);
    *out_capacity = capacity;
    return table;
}

/// @brief True if `needle`'s contact pair is present in the linear-probe table built by
///   contact_pair_table_build; stops at the first empty slot (open-addressing absence).
/// @param table Contact-pair hash table to search.
/// @param capacity Power-of-two number of table slots.
/// @param needle Contact record whose pair identity is queried.
/// @return One when the pair is present, otherwise zero.
int contact_pair_table_contains(const contact_pair_hash_entry *table,
                                int32_t capacity,
                                const rt_contact3d *needle) {
    uintptr_t hash;
    int32_t slot;
    if (!table || capacity <= 0 || !needle)
        return 0;
    hash = contact_pair_hash_value(needle);
    slot = (int32_t)(hash & (uintptr_t)(capacity - 1));
    for (int32_t probe = 0; probe < capacity; ++probe) {
        int32_t idx = (slot + probe) & (capacity - 1);
        if (!table[idx].contact)
            return 0;
        if (table[idx].hash == hash && contact_pair_equals(table[idx].contact, needle))
            return 1;
    }
    return 0;
}

/// @brief Return the stored contact whose (body_a,body_b,collider_a,collider_b) pair matches
///   @p needle, or NULL if absent. With one manifold per pair the match is unique, so this
///   returns the same contact a linear scan would — used to O(1) warm-start the solver.
/// @param table Contact-pair hash table to search.
/// @param capacity Power-of-two number of table slots.
/// @param needle Contact record whose pair identity is queried.
/// @return Borrowed matching contact record, or NULL when absent.
const rt_contact3d *contact_pair_table_find(const contact_pair_hash_entry *table,
                                            int32_t capacity,
                                            const rt_contact3d *needle) {
    uintptr_t hash;
    int32_t slot;
    if (!table || capacity <= 0 || !needle)
        return NULL;
    hash = contact_pair_hash_value(needle);
    slot = (int32_t)(hash & (uintptr_t)(capacity - 1));
    for (int32_t probe = 0; probe < capacity; ++probe) {
        int32_t idx = (slot + probe) & (capacity - 1);
        if (!table[idx].contact)
            return NULL;
        if (table[idx].hash == hash && contact_pair_equals(table[idx].contact, needle))
            return table[idx].contact;
    }
    return NULL;
}

/// @brief Compute the support point for a collider in a given direction.
///
/// "Support" = farthest point on the shape along `direction`. Closed
/// form for spheres and capsules; recursive for compound colliders
/// (pick the child whose support has the largest dot with `direction`);
/// AABB-based fallback for unrecognized shapes. Used by contact-point
/// reconstruction after narrow-phase collision detection.
/// @param collider Collider3D handle whose support is evaluated.
/// @param pose World pose applied to @p collider.
/// @param direction Three-component world direction.
/// @param out_point Three-component output receiving the farthest world point.
static void collider_support_point(void *collider,
                                   const rt_collider_pose *pose,
                                   const double *direction,
                                   double *out_point) {
    double dir[3];
    double mn[3];
    double mx[3];
    int64_t type;
    if (!out_point) {
        return;
    }
    vec3_set(out_point, 0.0, 0.0, 0.0);
    if (!collider || !pose || !direction)
        return;

    vec3_copy(dir, direction);
    if (vec3_normalize_in_place(dir) <= 1e-12)
        vec3_set(dir, 0.0, 1.0, 0.0);
    type = rt_collider3d_get_type(collider);

    switch (type) {
        case RT_COLLIDER3D_TYPE_BOX: {
            double half_extents[3];
            double inv_rotation[4];
            double local_dir[3];
            double local[3];
            rt_collider3d_get_box_half_extents_raw(collider, half_extents);
            quat_conjugate(pose->rotation, inv_rotation);
            quat_rotate_vec3(inv_rotation, dir, local_dir);
            local[0] = local_dir[0] * pose->scale[0] >= 0.0 ? half_extents[0] : -half_extents[0];
            local[1] = local_dir[1] * pose->scale[1] >= 0.0 ? half_extents[1] : -half_extents[1];
            local[2] = local_dir[2] * pose->scale[2] >= 0.0 ? half_extents[2] : -half_extents[2];
            transform_point_from_pose(pose, local, out_point);
            return;
        }
        case RT_COLLIDER3D_TYPE_SPHERE: {
            double radius = rt_collider3d_get_radius_raw(collider);
            double max_scale = fabs(pose->scale[0]);
            if (fabs(pose->scale[1]) > max_scale)
                max_scale = fabs(pose->scale[1]);
            if (fabs(pose->scale[2]) > max_scale)
                max_scale = fabs(pose->scale[2]);
            out_point[0] = pose->position[0] + dir[0] * radius * max_scale;
            out_point[1] = pose->position[1] + dir[1] * radius * max_scale;
            out_point[2] = pose->position[2] + dir[2] * radius * max_scale;
            return;
        }
        case RT_COLLIDER3D_TYPE_CAPSULE: {
            double radius = rt_collider3d_get_radius_raw(collider);
            double half_axis = fmax(rt_collider3d_get_height_raw(collider) * 0.5 - radius, 0.0);
            double sx = fabs(pose->scale[0]);
            double sy = fabs(pose->scale[1]);
            double sz = fabs(pose->scale[2]);
            double max_radial_scale = sx > sz ? sx : sz;
            double axis_dir[3];
            double local_y[3] = {0.0, 1.0, 0.0};
            quat_rotate_vec3(pose->rotation, local_y, axis_dir);
            if (vec3_normalize_in_place(axis_dir) <= 1e-12)
                vec3_set(axis_dir, 0.0, 1.0, 0.0);
            double side = vec3_dot(dir, axis_dir) >= 0.0 ? 1.0 : -1.0;
            out_point[0] = pose->position[0] + axis_dir[0] * half_axis * sy * side +
                           dir[0] * radius * max_radial_scale;
            out_point[1] = pose->position[1] + axis_dir[1] * half_axis * sy * side +
                           dir[1] * radius * max_radial_scale;
            out_point[2] = pose->position[2] + axis_dir[2] * half_axis * sy * side +
                           dir[2] * radius * max_radial_scale;
            return;
        }
        case RT_COLLIDER3D_TYPE_CONVEX_HULL: {
            rt_mesh3d *mesh = (rt_mesh3d *)rt_collider3d_get_mesh_raw(collider);
            double best_dot = -DBL_MAX;
            if (mesh && mesh->vertex_count > 0) {
                /* Argmax in LOCAL space: with world = R*(S o v) + T,
                 * dot(world, d) = dot(v, S o (R^-1 d)) + const, so scanning raw
                 * local vertices against the once-transformed direction picks the
                 * exact same vertex while skipping a full pose transform per
                 * vertex — this is GJK/EPA's hottest inner loop (30-80 support
                 * calls per pair per substep). Only the winner is transformed. */
                double inv_rotation[4];
                double local_dir[3];
                uint32_t best_index = 0;
                rt_mesh3d_refresh_bounds(mesh);
                quat_conjugate(pose->rotation, inv_rotation);
                quat_rotate_vec3(inv_rotation, dir, local_dir);
                local_dir[0] *= pose->scale[0];
                local_dir[1] *= pose->scale[1];
                local_dir[2] *= pose->scale[2];
                for (uint32_t i = 0; i < mesh->vertex_count; ++i) {
                    double d = (double)mesh->vertices[i].pos[0] * local_dir[0] +
                               (double)mesh->vertices[i].pos[1] * local_dir[1] +
                               (double)mesh->vertices[i].pos[2] * local_dir[2];
                    if (d > best_dot) {
                        best_dot = d;
                        best_index = i;
                    }
                }
                if (best_dot > -DBL_MAX) {
                    double local[3] = {mesh->vertices[best_index].pos[0],
                                       mesh->vertices[best_index].pos[1],
                                       mesh->vertices[best_index].pos[2]};
                    transform_point_from_pose(pose, local, out_point);
                    return;
                }
            }
            break;
        }
        case RT_COLLIDER3D_TYPE_COMPOUND: {
            int64_t child_count = rt_collider3d_get_child_count_raw(collider);
            double best_dot = -1e300;
            for (int64_t i = 0; i < child_count; ++i) {
                void *child = rt_collider3d_get_child_raw(collider, i);
                double child_pos[3], child_rot[4], child_scale[3];
                rt_collider_pose child_pose;
                double candidate[3];
                rt_collider3d_get_child_transform_raw(
                    collider, i, child_pos, child_rot, child_scale);
                collider_pose_compose(pose, child_pos, child_rot, child_scale, &child_pose);
                collider_support_point(child, &child_pose, dir, candidate);
                {
                    double d = vec3_dot(candidate, dir);
                    if (d > best_dot) {
                        best_dot = d;
                        vec3_copy(out_point, candidate);
                    }
                }
            }
            return;
        }
        default:
            rt_collider3d_compute_world_aabb_raw(
                collider, pose->position, pose->rotation, pose->scale, mn, mx);
            out_point[0] = dir[0] >= 0.0 ? mx[0] : mn[0];
            out_point[1] = dir[1] >= 0.0 ? mx[1] : mn[1];
            out_point[2] = dir[2] >= 0.0 ? mx[2] : mn[2];
            return;
    }
}

/// @brief Estimate a representative contact point given two leaf colliders + a normal.
///
/// Uses the overlapping region of the two world AABBs to pick a point on
/// the active contact face. This is deliberately conservative: for large
/// static boxes (floors, walls) a support point along an axis-aligned normal
/// can land on a far corner when the other normal components are zero, which
/// creates bogus torque in the contact solver.
/// @param a_leaf First leaf Collider3D.
/// @param a_pose World pose of @p a_leaf.
/// @param b_leaf Second leaf Collider3D.
/// @param b_pose World pose of @p b_leaf.
/// @param normal Three-component A-to-B contact normal.
/// @param point_out Three-component output receiving the representative midpoint.
static void compute_contact_point_from_leafs(void *a_leaf,
                                             const rt_collider_pose *a_pose,
                                             void *b_leaf,
                                             const rt_collider_pose *b_pose,
                                             const double *normal,
                                             double *point_out) {
    double point_a[3];
    double point_b[3];
    double inv_normal[3];
    double amn[3];
    double amx[3];
    double bmn[3];
    double bmx[3];
    double abs_n[3];
    int axis = 0;
    if (!point_out) {
        return;
    }
    vec3_set(point_out, 0.0, 0.0, 0.0);
    if (!a_leaf || !a_pose || !b_leaf || !b_pose || !normal)
        return;

    rt_collider3d_compute_world_aabb_raw(
        a_leaf, a_pose->position, a_pose->rotation, a_pose->scale, amn, amx);
    rt_collider3d_compute_world_aabb_raw(
        b_leaf, b_pose->position, b_pose->rotation, b_pose->scale, bmn, bmx);

    abs_n[0] = fabs(normal[0]);
    abs_n[1] = fabs(normal[1]);
    abs_n[2] = fabs(normal[2]);
    if (abs_n[1] > abs_n[axis])
        axis = 1;
    if (abs_n[2] > abs_n[axis])
        axis = 2;

    if (isfinite(amn[0]) && isfinite(amn[1]) && isfinite(amn[2]) && isfinite(amx[0]) &&
        isfinite(amx[1]) && isfinite(amx[2]) && isfinite(bmn[0]) && isfinite(bmn[1]) &&
        isfinite(bmn[2]) && isfinite(bmx[0]) && isfinite(bmx[1]) && isfinite(bmx[2])) {
        for (int i = 0; i < 3; ++i) {
            double lo = fmax(amn[i], bmn[i]);
            double hi = fmin(amx[i], bmx[i]);
            if (i == axis && abs_n[i] > 1e-8) {
                double a_face = normal[i] >= 0.0 ? amx[i] : amn[i];
                double b_face = normal[i] >= 0.0 ? bmn[i] : bmx[i];
                point_out[i] = (a_face + b_face) * 0.5;
            } else if (lo <= hi) {
                point_out[i] = (lo + hi) * 0.5;
            } else {
                double ac = (amn[i] + amx[i]) * 0.5;
                double bc = (bmn[i] + bmx[i]) * 0.5;
                point_out[i] = (ac + bc) * 0.5;
            }
        }
        return;
    }

    vec3_negate(normal, inv_normal);
    collider_support_point(a_leaf, a_pose, normal, point_a);
    collider_support_point(b_leaf, b_pose, inv_normal, point_b);
    point_out[0] = (point_a[0] + point_b[0]) * 0.5;
    point_out[1] = (point_a[1] + point_b[1]) * 0.5;
    point_out[2] = (point_a[2] + point_b[2]) * 0.5;
}

/// @brief Estimate a representative contact point from two bodies' overlapping world AABBs.
/// @param a First Body3D, establishing the normal origin.
/// @param b Second Body3D, toward which @p normal points.
/// @param normal Three-component A-to-B contact normal.
/// @param point_out Three-component output receiving the face-aware overlap midpoint.
static void compute_contact_point_from_body_aabbs(const rt_body3d *a,
                                                  const rt_body3d *b,
                                                  const double *normal,
                                                  double *point_out) {
    double amn[3], amx[3], bmn[3], bmx[3];
    double abs_n[3];
    int axis = 0;
    if (!point_out)
        return;
    vec3_set(point_out, 0.0, 0.0, 0.0);
    if (!a || !b || !normal)
        return;
    body_aabb(a, amn, amx);
    body_aabb(b, bmn, bmx);
    abs_n[0] = fabs(normal[0]);
    abs_n[1] = fabs(normal[1]);
    abs_n[2] = fabs(normal[2]);
    if (abs_n[1] > abs_n[axis])
        axis = 1;
    if (abs_n[2] > abs_n[axis])
        axis = 2;
    for (int i = 0; i < 3; ++i) {
        double lo = fmax(amn[i], bmn[i]);
        double hi = fmin(amx[i], bmx[i]);
        if (i == axis && abs_n[i] > 1e-8) {
            double a_face = normal[i] >= 0.0 ? amx[i] : amn[i];
            double b_face = normal[i] >= 0.0 ? bmn[i] : bmx[i];
            point_out[i] = (a_face + b_face) * 0.5;
        } else if (lo <= hi) {
            point_out[i] = (lo + hi) * 0.5;
        } else {
            double ac = (amn[i] + amx[i]) * 0.5;
            double bc = (bmn[i] + bmx[i]) * 0.5;
            point_out[i] = (ac + bc) * 0.5;
        }
    }
}

/// @brief Bidirectional layer/mask filter for body-vs-body collision.
///
/// Both bodies must be on a layer the other body's mask accepts. This
/// preserves intuitive behavior: e.g., players (layer P, mask=P|E) and
/// enemies (layer E, mask=P|E) collide; bullets (layer B, mask=E) hit
/// enemies but not players.
/// @param a First Body3D to filter.
/// @param b Second Body3D to filter.
/// @return One when each body's mask accepts the other's layer, otherwise zero.
int bodies_can_collide(const rt_body3d *a, const rt_body3d *b) {
    if (!a || !b)
        return 0;
    if (!(a->collision_layer & b->collision_mask))
        return 0;
    if (!(b->collision_layer & a->collision_mask))
        return 0;
    return 1;
}
