//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/physics/rt_physics3d_collision_narrowphase.inc  (#included from rt_physics3d_collision.c; not a standalone translation unit)
// Purpose: Physics3D shape-specific narrow-phase primitive collision tests
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d_collision_narrowphase.inc
 * @brief Implements primitive, oriented-box, mesh, capsule, and triangle collision tests.
 *
 * This fragment supplies the exact shape-level tests used by collider-pair
 * dispatch. All successful tests return a normal from A toward B and a
 * non-negative penetration depth, use bounded mesh traversal with safe
 * fallbacks, and preserve oriented collider poses.
 */

/* --- Shape-specific narrow-phase collision tests ---
 * Normal always points A→B (matches impulse convention: a.vel -= j*n). */

/// @brief AABB-vs-AABB narrow phase: returns penetration depth and normal.
///
/// SAT picks the axis of minimum overlap as the separation direction.
/// Normal points from A toward B (impulse convention: `a.vel -= j*n`).
/// @param a First AABB proxy body.
/// @param b Second AABB proxy body.
/// @param normal Three-component output receiving the A-to-B minimum-overlap axis.
/// @param depth Output receiving penetration depth.
/// @return One when the boxes overlap with positive depth, otherwise zero.
static int test_aabb_aabb(const rt_body3d *a, const rt_body3d *b, double *normal, double *depth) {
    double amn[3], amx[3], bmn[3], bmx[3];
    body_aabb(a, amn, amx);
    body_aabb(b, bmn, bmx);
    double ox = (amx[0] < bmx[0] ? amx[0] - bmn[0] : bmx[0] - amn[0]);
    double oy = (amx[1] < bmx[1] ? amx[1] - bmn[1] : bmx[1] - amn[1]);
    double oz = (amx[2] < bmx[2] ? amx[2] - bmn[2] : bmx[2] - amn[2]);
    if (ox <= 0 || oy <= 0 || oz <= 0)
        return 0;
    /* Sign the normal from the overlapping boxes' face midpoints, not the body
     * origins: colliders can be offset from the body center, and origins can
     * tie exactly. AABB centers describe the actual overlap geometry. */
    double acx = (amn[0] + amx[0]) * 0.5, bcx = (bmn[0] + bmx[0]) * 0.5;
    double acy = (amn[1] + amx[1]) * 0.5, bcy = (bmn[1] + bmx[1]) * 0.5;
    double acz = (amn[2] + amx[2]) * 0.5, bcz = (bmn[2] + bmx[2]) * 0.5;
    normal[0] = normal[1] = normal[2] = 0;
    if (ox <= oy && ox <= oz) {
        *depth = ox;
        normal[0] = (acx <= bcx) ? 1.0 : -1.0;
    } else if (oy <= oz) {
        *depth = oy;
        normal[1] = (acy <= bcy) ? 1.0 : -1.0;
    } else {
        *depth = oz;
        normal[2] = (acz <= bcz) ? 1.0 : -1.0;
    }
    return 1;
}

/// @brief Sphere-vs-sphere: simple distance vs sum-of-radii test.
///
/// Falls back to a +Y push when the centers are exactly coincident
/// (otherwise the normal would be ill-defined and the impulse step
/// would NaN out).
/// @param a First sphere proxy body.
/// @param b Second sphere proxy body.
/// @param normal Three-component output receiving the A-to-B unit normal.
/// @param depth Output receiving radial penetration depth.
/// @return One when the spheres overlap, otherwise zero.
static int test_sphere_sphere(const rt_body3d *a,
                              const rt_body3d *b,
                              double *normal,
                              double *depth) {
    double dx = b->position[0] - a->position[0];
    double dy = b->position[1] - a->position[1];
    double dz = b->position[2] - a->position[2];
    double dist_sq = dx * dx + dy * dy + dz * dz;
    double sum_r = a->radius + b->radius;
    if (dist_sq >= sum_r * sum_r)
        return 0;
    double dist = sqrt(dist_sq);
    if (dist < 1e-12) {
        /* Coincident centers — push along Y */
        normal[0] = 0;
        normal[1] = 1;
        normal[2] = 0;
        *depth = sum_r;
    } else {
        double inv_dist = 1.0 / dist;
        normal[0] = dx * inv_dist;
        normal[1] = dy * inv_dist;
        normal[2] = dz * inv_dist;
        *depth = sum_r - dist;
    }
    return 1;
}

/// @brief AABB-vs-sphere: clamp sphere center into AABB, then sphere-distance test.
///
/// If the sphere's center lies inside the AABB, falls back to the
/// AABB-AABB pushout to avoid a degenerate zero-distance normal. The
/// returned normal points from AABB to sphere.
/// @param aabb Box proxy body.
/// @param sph Sphere proxy body.
/// @param normal Three-component output receiving the box-to-sphere normal.
/// @param depth Output receiving penetration depth.
/// @return One when the sphere overlaps the AABB, otherwise zero.
static int test_aabb_sphere(const rt_body3d *aabb,
                            const rt_body3d *sph,
                            double *normal,
                            double *depth) {
    /* Find closest point on AABB to sphere center */
    double closest[3];
    double amn[3], amx[3];
    body_aabb(aabb, amn, amx);
    for (int i = 0; i < 3; i++) {
        double v = sph->position[i];
        if (v < amn[i])
            v = amn[i];
        if (v > amx[i])
            v = amx[i];
        closest[i] = v;
    }
    double dx = sph->position[0] - closest[0];
    double dy = sph->position[1] - closest[1];
    double dz = sph->position[2] - closest[2];
    double dist_sq = dx * dx + dy * dy + dz * dz;
    if (dist_sq >= sph->radius * sph->radius)
        return 0;
    double dist = sqrt(dist_sq);
    if (dist < 1e-12) {
        /* Sphere center inside AABB — use AABB pushout */
        return test_aabb_aabb(aabb, sph, normal, depth);
    }
    double inv_dist = 1.0 / dist;
    /* Normal points from AABB toward sphere */
    normal[0] = dx * inv_dist;
    normal[1] = dy * inv_dist;
    normal[2] = dz * inv_dist;
    *depth = sph->radius - dist;
    return 1;
}

/// @brief Whether `type` is one of the primitive shapes the narrow phase handles directly.
/// @param type Collider3D type discriminator.
/// @return One for box, sphere, or capsule types, otherwise zero.
static int collider_type_is_simple(int64_t type) {
    return type == RT_COLLIDER3D_TYPE_BOX || type == RT_COLLIDER3D_TYPE_SPHERE ||
           type == RT_COLLIDER3D_TYPE_CAPSULE;
}

/// @brief Build a transient `rt_body3d` whose shape mirrors a simple collider.
///
/// Used during compound-vs-compound collision: each leaf-pair produces
/// a transient body so we can reuse the body-level narrow-phase tests.
/// Applies the pose's scale to extents/radius/height. Returns 0 for
/// non-simple shapes (the caller falls back to AABB-vs-AABB).
/// @param pose World pose applied to the collider.
/// @param collider Simple Collider3D handle to mirror.
/// @param out Caller-provided Body3D storage receiving the proxy.
/// @return One for a supported primitive collider, otherwise zero.
int build_simple_proxy(const rt_collider_pose *pose, void *collider, rt_body3d *out) {
    double hx[3];
    int64_t type;
    if (!pose || !collider || !out)
        return 0;
    double sx = fabs(pose->scale[0]);
    double sy = fabs(pose->scale[1]);
    double sz = fabs(pose->scale[2]);
    rt_body3d zero = {0};
    *out = zero;
    out->position[0] = pose->position[0];
    out->position[1] = pose->position[1];
    out->position[2] = pose->position[2];
    out->orientation[0] = pose->rotation[0];
    out->orientation[1] = pose->rotation[1];
    out->orientation[2] = pose->rotation[2];
    out->orientation[3] = pose->rotation[3];
    type = rt_collider3d_get_type(collider);
    switch (type) {
        case RT_COLLIDER3D_TYPE_BOX:
            out->shape = PH3D_SHAPE_AABB;
            rt_collider3d_get_box_half_extents_raw(collider, hx);
            out->half_extents[0] = hx[0] * sx;
            out->half_extents[1] = hx[1] * sy;
            out->half_extents[2] = hx[2] * sz;
            return 1;
        case RT_COLLIDER3D_TYPE_SPHERE:
            out->shape = PH3D_SHAPE_SPHERE;
            out->radius = rt_collider3d_get_radius_raw(collider);
            if (sy > sx)
                sx = sy;
            if (sz > sx)
                sx = sz;
            out->radius *= sx;
            return 1;
        case RT_COLLIDER3D_TYPE_CAPSULE:
            out->shape = PH3D_SHAPE_CAPSULE;
            {
                double raw_radius = rt_collider3d_get_radius_raw(collider);
                double raw_height = rt_collider3d_get_height_raw(collider);
                double scaled_radius = raw_radius * (sx > sz ? sx : sz);
                double scaled_cylinder = fmax(raw_height - 2.0 * raw_radius, 0.0) * sy;
                out->radius = scaled_radius;
                out->height = scaled_cylinder + 2.0 * scaled_radius;
            }
            return 1;
        default:
            return 0;
    }
}

/// @brief Compute a box collider's world half-extents by scaling its raw local
///   half-extents by the pose's absolute per-axis scale.
/// @param collider Box Collider3D whose local extents are read.
/// @param pose World pose providing scale.
/// @param half_extents Three-component output receiving scaled half-extents.
static void box_scaled_half_extents(void *collider,
                                    const rt_collider_pose *pose,
                                    double *half_extents) {
    double raw[3];
    rt_collider3d_get_box_half_extents_raw(collider, raw);
    half_extents[0] = raw[0] * pose_abs_scale_or_zero(pose ? pose->scale[0] : 1.0);
    half_extents[1] = raw[1] * pose_abs_scale_or_zero(pose ? pose->scale[1] : 1.0);
    half_extents[2] = raw[2] * pose_abs_scale_or_zero(pose ? pose->scale[2] : 1.0);
}

/// @brief Derive the pose's three orthonormal world axes (rows of its rotation) by
///   rotating the local basis vectors; degenerate axes fall back to the identity basis.
/// @param pose World pose whose rotation defines the basis.
/// @param axes Three-by-three output receiving normalized world axes.
static void pose_rotation_axes(const rt_collider_pose *pose, double axes[3][3]) {
    const double local_x[3] = {1.0, 0.0, 0.0};
    const double local_y[3] = {0.0, 1.0, 0.0};
    const double local_z[3] = {0.0, 0.0, 1.0};
    quat_rotate_vec3(pose->rotation, local_x, axes[0]);
    quat_rotate_vec3(pose->rotation, local_y, axes[1]);
    quat_rotate_vec3(pose->rotation, local_z, axes[2]);
    for (int i = 0; i < 3; i++) {
        if (vec3_normalize_in_place(axes[i]) <= 1e-12) {
            axes[i][0] = axes[i][1] = axes[i][2] = 0.0;
            axes[i][i] = 1.0;
        }
    }
}

/// @brief SAT helper: if @p penetration is the smallest seen so far, record it and the
///   signed separating axis as the current best (minimum-penetration) result.
/// @param axis Three-component candidate unit axis.
/// @param sign Direction sign orienting the axis from A toward B.
/// @param penetration Candidate overlap depth.
/// @param best_penetration In/out smallest overlap depth seen.
/// @param best_axis Three-component output paired with @p best_penetration.
static void obb_record_axis(const double *axis,
                            double sign,
                            double penetration,
                            double *best_penetration,
                            double *best_axis) {
    if (penetration >= *best_penetration)
        return;
    *best_penetration = penetration;
    best_axis[0] = axis[0] * sign;
    best_axis[1] = axis[1] * sign;
    best_axis[2] = axis[2] * sign;
}

/// @brief Exact SAT test for two oriented box colliders.
/// @param a_collider First box Collider3D.
/// @param a_pose World pose of @p a_collider.
/// @param b_collider Second box Collider3D.
/// @param b_pose World pose of @p b_collider.
/// @param normal Three-component output receiving the A-to-B minimum-overlap axis.
/// @param depth Output receiving penetration depth in world units.
/// @return One when no SAT axis separates the boxes, otherwise zero.
static int test_box_box_obb(void *a_collider,
                            const rt_collider_pose *a_pose,
                            void *b_collider,
                            const rt_collider_pose *b_pose,
                            double *normal,
                            double *depth) {
    double a_he[3], b_he[3];
    double A[3][3], B[3][3];
    double R[3][3], AbsR[3][3];
    double t_world[3], t[3];
    double best_depth = DBL_MAX;
    double best_axis[3] = {1.0, 0.0, 0.0};
    const double eps = 1e-9;

    if (!a_collider || !a_pose || !b_collider || !b_pose || !normal || !depth)
        return 0;

    box_scaled_half_extents(a_collider, a_pose, a_he);
    box_scaled_half_extents(b_collider, b_pose, b_he);
    pose_rotation_axes(a_pose, A);
    pose_rotation_axes(b_pose, B);

    vec3_sub(b_pose->position, a_pose->position, t_world);
    for (int i = 0; i < 3; i++) {
        t[i] = vec3_dot(t_world, A[i]);
        for (int j = 0; j < 3; j++) {
            R[i][j] = vec3_dot(A[i], B[j]);
            AbsR[i][j] = fabs(R[i][j]) + eps;
        }
    }

    for (int i = 0; i < 3; i++) {
        double ra = a_he[i];
        double rb = b_he[0] * AbsR[i][0] + b_he[1] * AbsR[i][1] + b_he[2] * AbsR[i][2];
        double dist = fabs(t[i]);
        double penetration = ra + rb - dist;
        if (penetration < 0.0)
            return 0;
        obb_record_axis(A[i], t[i] >= 0.0 ? 1.0 : -1.0, penetration, &best_depth, best_axis);
    }

    for (int j = 0; j < 3; j++) {
        double ra = a_he[0] * AbsR[0][j] + a_he[1] * AbsR[1][j] + a_he[2] * AbsR[2][j];
        double rb = b_he[j];
        double dist = fabs(t[0] * R[0][j] + t[1] * R[1][j] + t[2] * R[2][j]);
        double penetration = ra + rb - dist;
        if (penetration < 0.0)
            return 0;
        {
            double sign = vec3_dot(t_world, B[j]) >= 0.0 ? 1.0 : -1.0;
            obb_record_axis(B[j], sign, penetration, &best_depth, best_axis);
        }
    }

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            double axis[3];
            double axis_len;
            double ra;
            double rb;
            double dist;
            double penetration;
            vec3_cross(A[i], B[j], axis);
            axis_len = vec3_normalize_in_place(axis);
            if (axis_len <= 1e-8)
                continue;
            ra =
                a_he[(i + 1) % 3] * AbsR[(i + 2) % 3][j] + a_he[(i + 2) % 3] * AbsR[(i + 1) % 3][j];
            rb =
                b_he[(j + 1) % 3] * AbsR[i][(j + 2) % 3] + b_he[(j + 2) % 3] * AbsR[i][(j + 1) % 3];
            dist = fabs(t[(i + 2) % 3] * R[(i + 1) % 3][j] - t[(i + 1) % 3] * R[(i + 2) % 3][j]);
            penetration = ra + rb - dist;
            if (penetration < 0.0)
                return 0;
            /* ra/rb/dist come from the R-matrix identities for the UNNORMALIZED
             * cross axis, so the overlap is measured in |A_i x B_j|-scaled units.
             * Rescale to true world units before comparing against the face axes,
             * or edge axes (|cross| = sin(theta) <= 1) always look artificially
             * shallow: the minimal-axis pick biases toward edges and the reported
             * depth under-resolves edge-edge contacts. */
            penetration /= axis_len;
            obb_record_axis(axis,
                            vec3_dot(t_world, axis) >= 0.0 ? 1.0 : -1.0,
                            penetration,
                            &best_depth,
                            best_axis);
        }
    }

    if (best_depth == DBL_MAX) {
        /* Defensive: no axis recorded a penetration. The six face-axis tests above
         * always record (they have no skip path) and return early on separation, so
         * this is unreachable today — but if a future edit adds a skip to the face
         * loops, report no-contact rather than fabricating a 0-depth +Y contact that
         * the solver would treat as a real (mis-normalled) collision. */
        *depth = 0.0;
        return 0;
    }
    vec3_copy(normal, best_axis);
    if (vec3_normalize_in_place(normal) <= 1e-12)
        vec3_set(normal, 0.0, 1.0, 0.0);
    *depth = best_depth;
    return *depth >= 0.0;
}

/// @brief Box-vs-sphere using the box's full oriented pose.
/// @param box_collider Box Collider3D.
/// @param box_pose World pose of @p box_collider.
/// @param sphere Sphere proxy body.
/// @param normal Three-component output receiving the box-to-sphere normal.
/// @param depth Output receiving penetration depth.
/// @return One when the sphere overlaps the oriented box, otherwise zero.
static int test_box_sphere_pose(void *box_collider,
                                const rt_collider_pose *box_pose,
                                const rt_body3d *sphere,
                                double *normal,
                                double *depth) {
    double raw_he[3];
    double local_center[3];
    double local_closest[3];
    double world_closest[3];
    double delta[3];
    double dist_sq;
    if (!box_collider || !box_pose || !sphere || !normal || !depth)
        return 0;
    rt_collider3d_get_box_half_extents_raw(box_collider, raw_he);
    transform_point_to_local(box_pose, sphere->position, local_center);
    for (int i = 0; i < 3; i++)
        local_closest[i] = clampd(local_center[i], -raw_he[i], raw_he[i]);
    transform_point_from_pose(box_pose, local_closest, world_closest);
    vec3_sub(sphere->position, world_closest, delta);
    dist_sq = vec3_len_sq(delta);
    if (dist_sq > sphere->radius * sphere->radius)
        return 0;
    if (dist_sq > 1e-18) {
        double dist = sqrt(dist_sq);
        normal[0] = delta[0] / dist;
        normal[1] = delta[1] / dist;
        normal[2] = delta[2] / dist;
        *depth = sphere->radius - dist;
        return 1;
    }

    {
        int axis = 0;
        double face_distance[3];
        double local_normal[3] = {0.0, 0.0, 0.0};
        for (int i = 0; i < 3; i++) {
            face_distance[i] =
                (raw_he[i] - fabs(local_center[i])) * pose_abs_scale_or_unit(box_pose->scale[i]);
            if (face_distance[i] < face_distance[axis])
                axis = i;
        }
        local_normal[axis] = local_center[axis] >= 0.0 ? 1.0 : -1.0;
        transform_normal_from_local(box_pose, local_normal, normal);
        *depth = sphere->radius + fmax(face_distance[axis], 0.0);
        return 1;
    }
}

/// @brief Box-vs-capsule via adaptive sphere samples along the capsule axis.
/// @param box_collider Box Collider3D.
/// @param box_pose World pose of @p box_collider.
/// @param capsule Capsule proxy body.
/// @param normal Three-component output receiving the deepest box-to-capsule normal.
/// @param depth Output receiving the deepest sampled penetration.
/// @return One when any capsule-axis sample overlaps the box, otherwise zero.
static int test_box_capsule_pose(void *box_collider,
                                 const rt_collider_pose *box_pose,
                                 const rt_body3d *capsule,
                                 double *normal,
                                 double *depth) {
    double a[3], b[3], axis[3];
    double best_depth = 0.0;
    double best_normal[3] = {0.0, 1.0, 0.0};
    int hit = 0;
    int samples;
    if (!box_collider || !box_pose || !capsule)
        return 0;
    capsule_axis_endpoints(capsule, a, b);
    vec3_sub(b, a, axis);
    samples = capsule_axis_sample_count(vec3_len(axis), capsule->radius);
    for (int i = 0; i < samples; i++) {
        double t = samples == 1 ? 0.5 : (double)i / (double)(samples - 1);
        rt_body3d sphere;
        double cur_normal[3];
        double cur_depth;
        make_temp_sphere(&sphere,
                         (double[3]){a[0] + axis[0] * t, a[1] + axis[1] * t, a[2] + axis[2] * t},
                         capsule->radius);
        if (test_box_sphere_pose(box_collider, box_pose, &sphere, cur_normal, &cur_depth) &&
            (!hit || cur_depth > best_depth)) {
            best_depth = cur_depth;
            vec3_copy(best_normal, cur_normal);
            hit = 1;
        }
    }
    if (!hit)
        return 0;
    vec3_copy(normal, best_normal);
    *depth = best_depth;
    return 1;
}

/// @brief Narrow-phase dispatch for two posed primitive colliders (box/sphere/capsule):
///   routes to the matching box-box/box-sphere/box-capsule (etc.) test, flipping the
///   returned normal so it always points from A toward B. Returns 1 on overlap with
///   @p normal/@p depth filled, else 0.
/// @param a_collider First primitive Collider3D.
/// @param a_pose World pose of @p a_collider.
/// @param b_collider Second primitive Collider3D.
/// @param b_pose World pose of @p b_collider.
/// @param normal Three-component output receiving the A-to-B contact normal.
/// @param depth Output receiving penetration depth.
/// @return One when the supported primitive pair overlaps, otherwise zero.
static int test_simple_collider_pose_collision(void *a_collider,
                                               const rt_collider_pose *a_pose,
                                               void *b_collider,
                                               const rt_collider_pose *b_pose,
                                               double *normal,
                                               double *depth) {
    int64_t a_type = rt_collider3d_get_type(a_collider);
    int64_t b_type = rt_collider3d_get_type(b_collider);
    rt_body3d proxy_a;
    rt_body3d proxy_b;

    if (a_type == RT_COLLIDER3D_TYPE_BOX && b_type == RT_COLLIDER3D_TYPE_BOX)
        return test_box_box_obb(a_collider, a_pose, b_collider, b_pose, normal, depth);

    if (a_type == RT_COLLIDER3D_TYPE_BOX) {
        if (!build_simple_proxy(b_pose, b_collider, &proxy_b))
            return 0;
        if (proxy_b.shape == PH3D_SHAPE_SPHERE)
            return test_box_sphere_pose(a_collider, a_pose, &proxy_b, normal, depth);
        if (proxy_b.shape == PH3D_SHAPE_CAPSULE)
            return test_box_capsule_pose(a_collider, a_pose, &proxy_b, normal, depth);
    }

    if (b_type == RT_COLLIDER3D_TYPE_BOX) {
        int hit;
        if (!build_simple_proxy(a_pose, a_collider, &proxy_a))
            return 0;
        if (proxy_a.shape == PH3D_SHAPE_SPHERE)
            hit = test_box_sphere_pose(b_collider, b_pose, &proxy_a, normal, depth);
        else if (proxy_a.shape == PH3D_SHAPE_CAPSULE)
            hit = test_box_capsule_pose(b_collider, b_pose, &proxy_a, normal, depth);
        else
            hit = 0;
        if (hit)
            vec3_negate(normal, normal);
        return hit;
    }

    if (!build_simple_proxy(a_pose, a_collider, &proxy_a) ||
        !build_simple_proxy(b_pose, b_collider, &proxy_b))
        return 0;
    return test_simple_collision(&proxy_a, &proxy_b, normal, depth);
}

/// @brief Narrow-phase dispatcher for primitive-shape body pairs.
///
/// Cheap broad-phase (AABB overlap) up front, then routes to:
///   - sphere/capsule × sphere/capsule via closest-axis sphere test
///   - AABB × sphere/capsule via clamp-then-distance
///   - AABB × AABB via SAT
/// The capsule shape is reduced to a transient sphere at the closest
/// axis point so the entire matrix collapses to ~3 specialized cases.
/// Always returns the normal pointing from A to B (so symmetric callers
/// flip it when needed).
///
/// CONTRACT: PH3D_SHAPE_AABB here is genuinely axis-aligned — a cached-shape
/// body's orientation is ignored for box collision (capsules DO honor it via
/// capsule_axis_endpoints). Every public Body3D constructor attaches a real
/// collider, whose box pairs take the pose-aware OBB SAT path instead; this
/// fallback only serves internal proxies and legacy cached-primitive bodies,
/// which never carry a rotation that matters.
/// @param a First primitive proxy body.
/// @param b Second primitive proxy body.
/// @param normal Three-component output receiving the A-to-B contact normal.
/// @param depth Output receiving penetration depth.
/// @return One when the primitive pair overlaps, otherwise zero.
static int test_simple_collision(const rt_body3d *a,
                                 const rt_body3d *b,
                                 double *normal,
                                 double *depth) {
    /* Broad phase: AABB overlap test */
    double amn[3], amx[3], bmn[3], bmx[3];
    body_aabb(a, amn, amx);
    body_aabb(b, bmn, bmx);
    if (amn[0] > bmx[0] || amx[0] < bmn[0] || amn[1] > bmx[1] || amx[1] < bmn[1] ||
        amn[2] > bmx[2] || amx[2] < bmn[2])
        return 0;

    /* Narrow phase: shape-specific dispatch */
    int sa = a->shape, sb = b->shape;

    /* Sphere/capsule pairs collapse to closest-axis sphere tests. */
    if ((sa == PH3D_SHAPE_SPHERE || sa == PH3D_SHAPE_CAPSULE) &&
        (sb == PH3D_SHAPE_SPHERE || sb == PH3D_SHAPE_CAPSULE)) {
        rt_body3d tmp_a, tmp_b;
        const rt_body3d *sphere_a = a;
        const rt_body3d *sphere_b = b;

        if (sa == PH3D_SHAPE_CAPSULE) {
            double center[3];
            if (sb == PH3D_SHAPE_CAPSULE) {
                double other_center[3];
                closest_points_capsule_axes(a, b, center, other_center);
                make_temp_sphere(&tmp_a, center, a->radius);
                make_temp_sphere(&tmp_b, other_center, b->radius);
                sphere_a = &tmp_a;
                sphere_b = &tmp_b;
                return test_sphere_sphere(sphere_a, sphere_b, normal, depth);
            }
            vec3_copy(center, b->position);
            closest_point_capsule_axis_to_point(a, center, center);
            make_temp_sphere(&tmp_a, center, a->radius);
            sphere_a = &tmp_a;
        }

        if (sb == PH3D_SHAPE_CAPSULE) {
            double center[3];
            vec3_copy(center, a->position);
            closest_point_capsule_axis_to_point(b, center, center);
            make_temp_sphere(&tmp_b, center, b->radius);
            sphere_b = &tmp_b;
        }
        return test_sphere_sphere(sphere_a, sphere_b, normal, depth);
    }

    /* AABB-sphere (order: A=AABB, B=sphere) */
    if (sa == PH3D_SHAPE_AABB && (sb == PH3D_SHAPE_SPHERE || sb == PH3D_SHAPE_CAPSULE)) {
        if (sb == PH3D_SHAPE_CAPSULE) {
            double center[3];
            rt_body3d tmp_sphere;
            closest_point_capsule_axis_to_aabb(b, a, center);
            make_temp_sphere(&tmp_sphere, center, b->radius);
            return test_aabb_sphere(a, &tmp_sphere, normal, depth);
        }
        return test_aabb_sphere(a, b, normal, depth);
    }

    /* Sphere-AABB (reversed — flip normal) */
    if ((sa == PH3D_SHAPE_SPHERE || sa == PH3D_SHAPE_CAPSULE) && sb == PH3D_SHAPE_AABB) {
        int hit;
        if (sa == PH3D_SHAPE_CAPSULE) {
            double center[3];
            rt_body3d tmp_sphere;
            closest_point_capsule_axis_to_aabb(a, b, center);
            make_temp_sphere(&tmp_sphere, center, a->radius);
            hit = test_aabb_sphere(b, &tmp_sphere, normal, depth);
        } else {
            hit = test_aabb_sphere(b, a, normal, depth);
        }
        if (hit) {
            normal[0] = -normal[0];
            normal[1] = -normal[1];
            normal[2] = -normal[2];
        }
        return hit;
    }

    /* AABB-AABB fallback */
    return test_aabb_aabb(a, b, normal, depth);
}

/// @brief Generic AABB-vs-AABB overlap with explicit centers (no body required).
///
/// Same SAT logic as `test_aabb_aabb` but takes raw extents and centers
/// so it can be used for compound child colliders that don't have a
/// backing body.
/// @param amn First AABB minimum corner.
/// @param amx First AABB maximum corner.
/// @param a_center First AABB center.
/// @param bmn Second AABB minimum corner.
/// @param bmx Second AABB maximum corner.
/// @param b_center Second AABB center.
/// @param normal Three-component output receiving the A-to-B minimum-overlap axis.
/// @param depth Output receiving penetration depth.
/// @return One when the raw bounds overlap with positive depth, otherwise zero.
static int test_bounds_overlap(const double *amn,
                               const double *amx,
                               const double *a_center,
                               const double *bmn,
                               const double *bmx,
                               const double *b_center,
                               double *normal,
                               double *depth) {
    double ox = (amx[0] < bmx[0] ? amx[0] - bmn[0] : bmx[0] - amn[0]);
    double oy = (amx[1] < bmx[1] ? amx[1] - bmn[1] : bmx[1] - amn[1]);
    double oz = (amx[2] < bmx[2] ? amx[2] - bmn[2] : bmx[2] - amn[2]);
    if (ox <= 0.0 || oy <= 0.0 || oz <= 0.0)
        return 0;
    normal[0] = normal[1] = normal[2] = 0.0;
    if (ox <= oy && ox <= oz) {
        *depth = ox;
        normal[0] = (a_center[0] < b_center[0]) ? 1.0 : -1.0;
    } else if (oy <= oz) {
        *depth = oy;
        normal[1] = (a_center[1] < b_center[1]) ? 1.0 : -1.0;
    } else {
        *depth = oz;
        normal[2] = (a_center[2] < b_center[2]) ? 1.0 : -1.0;
    }
    return 1;
}

/// @brief Project point `p` onto triangle `(a, b, c)`, writing the closest point.
///
/// Implements the standard Voronoi-region algorithm from Real-Time
/// Collision Detection (Ericson §5.1.5): tests whether `p` falls into
/// the vertex, edge, or interior region and clamps accordingly.
/// @param p Three-component query point.
/// @param a First triangle vertex.
/// @param b Second triangle vertex.
/// @param c Third triangle vertex.
/// @param closest Three-component output receiving the closest triangle point.
static void closest_point_on_triangle(
    const double *p, const double *a, const double *b, const double *c, double *closest) {
    double ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    double ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    double d1 = vec3_dot(ab, ap);
    double d2 = vec3_dot(ac, ap);
    double d3, d4, d5, d6;
    if (d1 <= 0.0 && d2 <= 0.0) {
        vec3_copy(closest, a);
        return;
    }

    {
        double bp[3] = {p[0] - b[0], p[1] - b[1], p[2] - b[2]};
        d3 = vec3_dot(ab, bp);
        d4 = vec3_dot(ac, bp);
        if (d3 >= 0.0 && d4 <= d3) {
            vec3_copy(closest, b);
            return;
        }
        {
            double vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
                double v = d1 / (d1 - d3);
                closest[0] = a[0] + ab[0] * v;
                closest[1] = a[1] + ab[1] * v;
                closest[2] = a[2] + ab[2] * v;
                return;
            }
        }
    }

    {
        double cp[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
        d5 = vec3_dot(ab, cp);
        d6 = vec3_dot(ac, cp);
        if (d6 >= 0.0 && d5 <= d6) {
            vec3_copy(closest, c);
            return;
        }
        {
            double vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
                double w = d2 / (d2 - d6);
                closest[0] = a[0] + ac[0] * w;
                closest[1] = a[1] + ac[1] * w;
                closest[2] = a[2] + ac[2] * w;
                return;
            }
        }
        {
            double va = d3 * d6 - d5 * d4;
            if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
                double bc[3] = {c[0] - b[0], c[1] - b[1], c[2] - b[2]};
                double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                closest[0] = b[0] + bc[0] * w;
                closest[1] = b[1] + bc[1] * w;
                closest[2] = b[2] + bc[2] * w;
                return;
            }
        }
    }

    {
        double denom_raw =
            (vec3_dot(ab, ab) * vec3_dot(ac, ac)) - vec3_dot(ab, ac) * vec3_dot(ab, ac);
        if (!isfinite(denom_raw) || fabs(denom_raw) < 1e-18) {
            /* Degenerate (zero-area / collinear) triangle: the Gram determinant is
             * singular, so the barycentric solve would divide by ~0 and inject NaN
             * into the contact normal/depth. Fall back to vertex a. */
            closest[0] = a[0];
            closest[1] = a[1];
            closest[2] = a[2];
            return;
        }
        double denom = 1.0 / denom_raw;
        double dot_ap_ab = vec3_dot(ap, ab);
        double dot_ap_ac = vec3_dot(ap, ac);
        double dot_ab_ab = vec3_dot(ab, ab);
        double dot_ab_ac = vec3_dot(ab, ac);
        double dot_ac_ac = vec3_dot(ac, ac);
        double v = (dot_ac_ac * dot_ap_ab - dot_ab_ac * dot_ap_ac) * denom;
        double w = (dot_ab_ab * dot_ap_ac - dot_ab_ac * dot_ap_ab) * denom;
        closest[0] = a[0] + ab[0] * v + ac[0] * w;
        closest[1] = a[1] + ab[1] * v + ac[1] * w;
        closest[2] = a[2] + ab[2] * v + ac[2] * w;
    }
}

/// @brief Compute a unit normal from three triangle vertices via cross product.
///
/// Falls back to +Y when the triangle is degenerate (zero area). The
/// orientation depends on vertex order — caller may flip if needed.
/// @param a First triangle vertex.
/// @param b Second triangle vertex.
/// @param c Third triangle vertex.
/// @param normal Three-component output receiving the unit face normal.
void triangle_normal(const double *a, const double *b, const double *c, double *normal) {
    double ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    double ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    normal[0] = ab[1] * ac[2] - ab[2] * ac[1];
    normal[1] = ab[2] * ac[0] - ab[0] * ac[2];
    normal[2] = ab[0] * ac[1] - ab[1] * ac[0];
    {
        double len = vec3_len(normal);
        if (len > 1e-12) {
            normal[0] /= len;
            normal[1] /= len;
            normal[2] /= len;
        } else {
            normal[0] = 0.0;
            normal[1] = 1.0;
            normal[2] = 0.0;
        }
    }
}

/// @brief Ensure the mesh's physics triangle BVH matches its current geometry revision.
/// @param mesh Mesh3D whose cached physics acceleration structure may rebuild.
/// @return One when a valid BVH is available, otherwise zero.
int mesh_physics_bvh_rebuild(rt_mesh3d *mesh);

/// @brief Transform a local-space AABB into a conservative world-space AABB.
/// @param pose World pose applied to the local bounds.
/// @param mn Three-component local minimum corner.
/// @param mx Three-component local maximum corner.
/// @param out_min Three-component output receiving the world minimum.
/// @param out_max Three-component output receiving the world maximum.
void transform_local_aabb_to_world(const rt_collider_pose *pose,
                                   const float *mn,
                                   const float *mx,
                                   double *out_min,
                                   double *out_max);

/// @brief Whether a sphere overlaps an axis-aligned box, via closest-point squared distance.
/// @details Sums squared distance only on axes where the center lies outside [mn, mx] and compares
///          to radius² — no sqrt, and an inside-the-box center trivially intersects.
/// @param mn AABB minimum corner.
/// @param mx AABB maximum corner.
/// @param center Sphere center.
/// @param radius Non-negative sphere radius.
/// @return One when the closed sphere intersects the AABB, otherwise zero.
static int aabb_intersects_sphere_raw(const double *mn,
                                      const double *mx,
                                      const double *center,
                                      double radius) {
    double dist_sq = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        double v = center[axis];
        if (v < mn[axis]) {
            double d = mn[axis] - v;
            dist_sq += d * d;
        } else if (v > mx[axis]) {
            double d = v - mx[axis];
            dist_sq += d * d;
        }
    }
    return dist_sq <= radius * radius;
}

/// @brief Transform a world-space AABB into a pose's local frame (bounding box of the
///   inverse-transformed corners).
/// @details Exact as a bound of the transformed box (an affine map sends the box to a
///   parallelepiped whose AABB is the corner min/max); testing stored local BVH node
///   bounds against this box is conservative, so pruning stays safe while each node
///   visit skips the previous 8-corner local->world transform.
/// @param pose Pose whose inverse transforms the world bounds.
/// @param mn World-space AABB minimum corner.
/// @param mx World-space AABB maximum corner.
/// @param out_min Output receiving the conservative local minimum.
/// @param out_max Output receiving the conservative local maximum.
static void ph3d_world_aabb_to_local_bounds(const rt_collider_pose *pose,
                                            const double *mn,
                                            const double *mx,
                                            double out_min[3],
                                            double out_max[3]) {
    for (int axis = 0; axis < 3; ++axis) {
        out_min[axis] = DBL_MAX;
        out_max[axis] = -DBL_MAX;
    }
    for (int i = 0; i < 8; ++i) {
        double world[3] = {
            (i & 1) ? mx[0] : mn[0], (i & 2) ? mx[1] : mn[1], (i & 4) ? mx[2] : mn[2]};
        double local[3];
        transform_point_to_local(pose, world, local);
        for (int axis = 0; axis < 3; ++axis) {
            if (local[axis] < out_min[axis])
                out_min[axis] = local[axis];
            if (local[axis] > out_max[axis])
                out_max[axis] = local[axis];
        }
    }
}

/// @brief Conservative local-space radius for a world-space sphere under a pose's scale.
/// @details A world sphere maps to a local ellipsoid with semi-axes radius/|scale_i|;
///   the bounding sphere of that ellipsoid has radius radius/min|scale_i|.
/// @param pose Pose whose absolute scale maps world to local space.
/// @param radius World-space sphere radius.
/// @return Conservative non-negative local-space radius.
static double ph3d_world_radius_to_local(const rt_collider_pose *pose, double radius) {
    double sx = pose_abs_scale_or_unit(pose->scale[0]);
    double sy = pose_abs_scale_or_unit(pose->scale[1]);
    double sz = pose_abs_scale_or_unit(pose->scale[2]);
    double min_scale = sx < sy ? sx : sy;
    if (sz < min_scale)
        min_scale = sz;
    if (!isfinite(min_scale) || min_scale <= 1e-12)
        return radius;
    return radius / min_scale;
}

/// @brief Sphere-vs-mesh narrow phase by per-triangle closest-point test.
///
/// Traverses the mesh's BVH entirely in mesh-local space (query sphere transformed
/// once) and tests only candidate leaf triangles, falling back to a full scan if
/// BVH traversal cannot complete.
/// @param mesh Triangle mesh to test.
/// @param mesh_pose World pose applied to @p mesh.
/// @param sphere Sphere proxy body.
/// @param normal Three-component output receiving the deepest mesh-to-sphere normal.
/// @param depth Output receiving the deepest penetration.
/// @return One when any triangle overlaps the sphere, otherwise zero.
static int test_meshlike_sphere(rt_mesh3d *mesh,
                                const rt_collider_pose *mesh_pose,
                                const rt_body3d *sphere,
                                double *normal,
                                double *depth) {
    double best_depth = 0.0;
    double best_normal[3] = {0.0, 1.0, 0.0};
    if (!mesh || !mesh_pose || !sphere || mesh->index_count < 3)
        return 0;
    rt_mesh3d_refresh_bounds(mesh);
    if (mesh_physics_bvh_rebuild(mesh)) {
        const rt_physics_mesh_bvh_node *nodes =
            (const rt_physics_mesh_bvh_node *)mesh->physics_bvh_nodes;
        const uint32_t *tri_indices = mesh->physics_bvh_tri_indices;
        int32_t *stack = NULL;
        int32_t top = 0;
        int32_t stack_capacity = 0;
        int overflow = 0;
        double local_center[3];
        double local_radius;
        transform_point_to_local(mesh_pose, sphere->position, local_center);
        local_radius = ph3d_world_radius_to_local(mesh_pose, sphere->radius);
        if (!ph3d_i32_stack_push(&stack, &top, &stack_capacity, 0)) {
            overflow = 1;
        }
        while (top > 0) {
            int32_t node_index = stack[--top];
            const rt_physics_mesh_bvh_node *node;
            double node_min[3], node_max[3];
            if (node_index < 0 || node_index >= mesh->physics_bvh_node_count)
                continue;
            node = &nodes[node_index];
            node_min[0] = node->min[0];
            node_min[1] = node->min[1];
            node_min[2] = node->min[2];
            node_max[0] = node->max[0];
            node_max[1] = node->max[1];
            node_max[2] = node->max[2];
            if (!aabb_intersects_sphere_raw(node_min, node_max, local_center, local_radius))
                continue;
            if (node->left >= 0 || node->right >= 0) {
                if (node->right >= 0 &&
                    !ph3d_i32_stack_push(&stack, &top, &stack_capacity, node->right)) {
                    overflow = 1;
                    break;
                }
                if (node->left >= 0 &&
                    !ph3d_i32_stack_push(&stack, &top, &stack_capacity, node->left)) {
                    overflow = 1;
                    break;
                }
                continue;
            }
            for (int32_t item = node->start; item < node->start + node->count; ++item) {
                uint32_t tri = tri_indices[item];
                uint32_t i0 = mesh->indices[tri * 3u + 0u];
                uint32_t i1 = mesh->indices[tri * 3u + 1u];
                uint32_t i2 = mesh->indices[tri * 3u + 2u];
                double a[3], b[3], c[3], closest[3];
                double dx, dy, dz, dist_sq;
                if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count ||
                    i2 >= mesh->vertex_count)
                    continue;
                {
                    double local[3];
                    local[0] = mesh->vertices[i0].pos[0];
                    local[1] = mesh->vertices[i0].pos[1];
                    local[2] = mesh->vertices[i0].pos[2];
                    transform_point_from_pose(mesh_pose, local, a);
                    local[0] = mesh->vertices[i1].pos[0];
                    local[1] = mesh->vertices[i1].pos[1];
                    local[2] = mesh->vertices[i1].pos[2];
                    transform_point_from_pose(mesh_pose, local, b);
                    local[0] = mesh->vertices[i2].pos[0];
                    local[1] = mesh->vertices[i2].pos[1];
                    local[2] = mesh->vertices[i2].pos[2];
                    transform_point_from_pose(mesh_pose, local, c);
                }
                closest_point_on_triangle(sphere->position, a, b, c, closest);
                dx = sphere->position[0] - closest[0];
                dy = sphere->position[1] - closest[1];
                dz = sphere->position[2] - closest[2];
                dist_sq = dx * dx + dy * dy + dz * dz;
                if (dist_sq >= sphere->radius * sphere->radius)
                    continue;
                {
                    double dist = sqrt(dist_sq);
                    double cur_depth = sphere->radius - dist;
                    double cur_normal[3];
                    if (dist > 1e-12) {
                        cur_normal[0] = dx / dist;
                        cur_normal[1] = dy / dist;
                        cur_normal[2] = dz / dist;
                    } else {
                        double centroid[3] = {(a[0] + b[0] + c[0]) / 3.0,
                                              (a[1] + b[1] + c[1]) / 3.0,
                                              (a[2] + b[2] + c[2]) / 3.0};
                        triangle_normal(a, b, c, cur_normal);
                        if ((sphere->position[0] - centroid[0]) * cur_normal[0] +
                                (sphere->position[1] - centroid[1]) * cur_normal[1] +
                                (sphere->position[2] - centroid[2]) * cur_normal[2] <
                            0.0) {
                            cur_normal[0] = -cur_normal[0];
                            cur_normal[1] = -cur_normal[1];
                            cur_normal[2] = -cur_normal[2];
                        }
                    }
                    if (cur_depth > best_depth) {
                        best_depth = cur_depth;
                        vec3_copy(best_normal, cur_normal);
                    }
                }
            }
        }
        free(stack);
        if (!overflow)
            goto mesh_sphere_done;
        best_depth = 0.0;
        vec3_set(best_normal, 0.0, 1.0, 0.0);
    }
    for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
        uint32_t i0 = mesh->indices[i];
        uint32_t i1 = mesh->indices[i + 1];
        uint32_t i2 = mesh->indices[i + 2];
        double a[3], b[3], c[3], closest[3];
        double dx, dy, dz, dist_sq;
        if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count || i2 >= mesh->vertex_count)
            continue;
        {
            double local[3];
            local[0] = mesh->vertices[i0].pos[0];
            local[1] = mesh->vertices[i0].pos[1];
            local[2] = mesh->vertices[i0].pos[2];
            transform_point_from_pose(mesh_pose, local, a);
            local[0] = mesh->vertices[i1].pos[0];
            local[1] = mesh->vertices[i1].pos[1];
            local[2] = mesh->vertices[i1].pos[2];
            transform_point_from_pose(mesh_pose, local, b);
            local[0] = mesh->vertices[i2].pos[0];
            local[1] = mesh->vertices[i2].pos[1];
            local[2] = mesh->vertices[i2].pos[2];
            transform_point_from_pose(mesh_pose, local, c);
        }
        closest_point_on_triangle(sphere->position, a, b, c, closest);
        dx = sphere->position[0] - closest[0];
        dy = sphere->position[1] - closest[1];
        dz = sphere->position[2] - closest[2];
        dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq >= sphere->radius * sphere->radius)
            continue;
        {
            double dist = sqrt(dist_sq);
            double cur_depth = sphere->radius - dist;
            double cur_normal[3];
            if (dist > 1e-12) {
                cur_normal[0] = dx / dist;
                cur_normal[1] = dy / dist;
                cur_normal[2] = dz / dist;
            } else {
                double centroid[3] = {(a[0] + b[0] + c[0]) / 3.0,
                                      (a[1] + b[1] + c[1]) / 3.0,
                                      (a[2] + b[2] + c[2]) / 3.0};
                triangle_normal(a, b, c, cur_normal);
                if ((sphere->position[0] - centroid[0]) * cur_normal[0] +
                        (sphere->position[1] - centroid[1]) * cur_normal[1] +
                        (sphere->position[2] - centroid[2]) * cur_normal[2] <
                    0.0) {
                    cur_normal[0] = -cur_normal[0];
                    cur_normal[1] = -cur_normal[1];
                    cur_normal[2] = -cur_normal[2];
                }
            }
            if (cur_depth > best_depth) {
                best_depth = cur_depth;
                vec3_copy(best_normal, cur_normal);
            }
        }
    }
mesh_sphere_done:
    if (best_depth <= 0.0)
        return 0;
    *depth = best_depth;
    vec3_copy(normal, best_normal);
    return 1;
}

/// @brief Capsule-vs-mesh narrow phase via adaptive sphere samples along the capsule axis.
///
/// Samples at radius-relative spacing instead of a fixed small count, which
/// avoids missing side contacts on long capsules.
/// @param mesh Triangle mesh to test.
/// @param mesh_pose World pose applied to @p mesh.
/// @param capsule Capsule proxy body.
/// @param normal Three-component output receiving the deepest mesh-to-capsule normal.
/// @param depth Output receiving the deepest sampled penetration.
/// @return One when any capsule-axis sample overlaps a triangle, otherwise zero.
static int test_meshlike_capsule(rt_mesh3d *mesh,
                                 const rt_collider_pose *mesh_pose,
                                 const rt_body3d *capsule,
                                 double *normal,
                                 double *depth) {
    double half_axis = fmax(capsule->height * 0.5 - capsule->radius, 0.0);
    double axis_a[3], axis_b[3];
    double best_depth = 0.0;
    double best_normal[3] = {0.0, 1.0, 0.0};
    int samples;
    capsule_axis_endpoints(capsule, axis_a, axis_b);
    {
        double axis_delta[3];
        vec3_sub(axis_b, axis_a, axis_delta);
        half_axis = vec3_len(axis_delta);
    }
    samples = capsule_axis_sample_count(half_axis, capsule->radius);
    for (int i = 0; i < samples; ++i) {
        double t = samples == 1 ? 0.5 : (double)i / (double)(samples - 1);
        rt_body3d sphere = {0};
        sphere.shape = PH3D_SHAPE_SPHERE;
        sphere.position[0] = axis_a[0] + (axis_b[0] - axis_a[0]) * t;
        sphere.position[1] = axis_a[1] + (axis_b[1] - axis_a[1]) * t;
        sphere.position[2] = axis_a[2] + (axis_b[2] - axis_a[2]) * t;
        sphere.radius = capsule->radius;
        {
            double cur_normal[3];
            double cur_depth;
            if (test_meshlike_sphere(mesh, mesh_pose, &sphere, cur_normal, &cur_depth) &&
                cur_depth > best_depth) {
                best_depth = cur_depth;
                vec3_copy(best_normal, cur_normal);
            }
        }
    }
    if (best_depth <= 0.0)
        return 0;
    *depth = best_depth;
    vec3_copy(normal, best_normal);
    return 1;
}

/// @brief Initialize a collider pose (identity scale) from a body's position and
///   orientation — used to bring a body proxy into the SAT routines.
/// @param body Body3D proxy whose position and orientation are copied.
/// @param pose Output receiving the corresponding unit-scale collider pose.
static void body_pose_from_proxy(const rt_body3d *body, rt_collider_pose *pose) {
    collider_pose_identity(pose);
    if (!body)
        return;
    vec3_copy(pose->position, body->position);
    pose->rotation[0] = body->orientation[0];
    pose->rotation[1] = body->orientation[1];
    pose->rotation[2] = body->orientation[2];
    pose->rotation[3] = body->orientation[3];
}

/// @brief One SAT axis test for a triangle vs a box (in the box's local frame): project
///   both onto the normalized @p axis_in; returns 0 if they are separated, else 1 and
///   updates the running minimum-overlap axis/sign. Zero-length axes are skipped (return 1).
/// @param tri Three triangle vertices in box-local space.
/// @param he Three-component box half-extents.
/// @param axis_in Candidate separating axis.
/// @param best_depth In/out smallest overlap found.
/// @param best_axis Three-component output paired with @p best_depth.
/// @param best_sign Output orienting @p best_axis out of the box toward the triangle.
/// @return Zero when the axis separates the shapes, otherwise one.
static int test_triangle_box_local_axis(const double tri[3][3],
                                        const double he[3],
                                        const double axis_in[3],
                                        double *best_depth,
                                        double *best_axis,
                                        double *best_sign) {
    double axis[3] = {axis_in[0], axis_in[1], axis_in[2]};
    double len = vec3_normalize_in_place(axis);
    double min_p;
    double max_p;
    double radius;
    double overlap;
    if (len <= 1e-10)
        return 1;
    min_p = max_p = vec3_dot(tri[0], axis);
    for (int i = 1; i < 3; i++) {
        double p = vec3_dot(tri[i], axis);
        if (p < min_p)
            min_p = p;
        if (p > max_p)
            max_p = p;
    }
    radius = he[0] * fabs(axis[0]) + he[1] * fabs(axis[1]) + he[2] * fabs(axis[2]);
    if (max_p < -radius || min_p > radius)
        return 0;
    overlap = fmin(radius - min_p, max_p + radius);
    if (overlap < *best_depth) {
        double centroid[3] = {(tri[0][0] + tri[1][0] + tri[2][0]) / 3.0,
                              (tri[0][1] + tri[1][1] + tri[2][1]) / 3.0,
                              (tri[0][2] + tri[1][2] + tri[2][2]) / 3.0};
        *best_depth = overlap;
        vec3_copy(best_axis, axis);
        *best_sign = vec3_dot(centroid, axis) <= 0.0 ? 1.0 : -1.0;
    }
    return 1;
}

/// @brief Full triangle-vs-oriented-box overlap test via the separating-axis theorem:
///   transforms the triangle into the box's local space and tests the 3 box axes, the
///   triangle normal, and the 9 edge cross-product axes. Returns 1 on overlap with the
///   world-space @p normal (pointing out of the box) and penetration @p depth.
/// @param world_a First world-space triangle vertex.
/// @param world_b Second world-space triangle vertex.
/// @param world_c Third world-space triangle vertex.
/// @param box Oriented box proxy body.
/// @param normal Three-component output receiving the box-to-triangle normal.
/// @param depth Output receiving minimum SAT penetration.
/// @return One when no candidate axis separates the triangle and box, otherwise zero.
static int test_triangle_box_obb(const double *world_a,
                                 const double *world_b,
                                 const double *world_c,
                                 const rt_body3d *box,
                                 double *normal,
                                 double *depth) {
    rt_collider_pose box_pose;
    double tri[3][3];
    double edge0[3], edge1[3], edge2[3];
    double tri_normal[3];
    double best_depth = DBL_MAX;
    double best_axis[3] = {0.0, 1.0, 0.0};
    double best_sign = 1.0;
    const double axes[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};

    if (!world_a || !world_b || !world_c || !box || !normal || !depth)
        return 0;
    body_pose_from_proxy(box, &box_pose);
    transform_point_to_local(&box_pose, world_a, tri[0]);
    transform_point_to_local(&box_pose, world_b, tri[1]);
    transform_point_to_local(&box_pose, world_c, tri[2]);

    vec3_sub(tri[1], tri[0], edge0);
    vec3_sub(tri[2], tri[1], edge1);
    vec3_sub(tri[0], tri[2], edge2);
    vec3_cross(edge0, edge1, tri_normal);

    for (int i = 0; i < 3; i++) {
        if (!test_triangle_box_local_axis(
                tri, box->half_extents, axes[i], &best_depth, best_axis, &best_sign))
            return 0;
    }
    if (!test_triangle_box_local_axis(
            tri, box->half_extents, tri_normal, &best_depth, best_axis, &best_sign))
        return 0;
    for (int e = 0; e < 3; e++) {
        const double *edge = e == 0 ? edge0 : (e == 1 ? edge1 : edge2);
        for (int a = 0; a < 3; a++) {
            double axis[3];
            vec3_cross(edge, axes[a], axis);
            if (!test_triangle_box_local_axis(
                    tri, box->half_extents, axis, &best_depth, best_axis, &best_sign))
                return 0;
        }
    }

    best_axis[0] *= best_sign;
    best_axis[1] *= best_sign;
    best_axis[2] *= best_sign;
    quat_rotate_vec3(box_pose.rotation, best_axis, normal);
    if (vec3_normalize_in_place(normal) <= 1e-12)
        vec3_set(normal, 0.0, 1.0, 0.0);
    *depth = best_depth == DBL_MAX ? 0.0 : best_depth;
    return 1;
}

/// @brief Test a posed triangle mesh against a box body by traversing mesh BVH
///   candidates before the triangle-vs-OBB SAT test. Falls back to a full scan if
///   BVH traversal cannot complete.
/// @param mesh Triangle mesh to test.
/// @param mesh_pose World pose applied to @p mesh.
/// @param box Oriented box proxy body.
/// @param normal Three-component output receiving the deepest mesh-to-box normal.
/// @param depth Output receiving the deepest triangle penetration.
/// @return One when any mesh triangle overlaps the box, otherwise zero.
static int test_meshlike_box(rt_mesh3d *mesh,
                             const rt_collider_pose *mesh_pose,
                             const rt_body3d *box,
                             double *normal,
                             double *depth) {
    double best_depth = 0.0;
    double best_normal[3] = {0.0, 1.0, 0.0};
    int hit = 0;
    if (!mesh || !mesh_pose || !box)
        return 0;
    rt_mesh3d_refresh_bounds(mesh);
    if (mesh_physics_bvh_rebuild(mesh)) {
        const rt_physics_mesh_bvh_node *nodes =
            (const rt_physics_mesh_bvh_node *)mesh->physics_bvh_nodes;
        const uint32_t *tri_indices = mesh->physics_bvh_tri_indices;
        double box_min[3], box_max[3];
        double local_query_min[3], local_query_max[3];
        int32_t stack[128];
        int32_t top = 0;
        int overflow = 0;
        body_aabb(box, box_min, box_max);
        ph3d_world_aabb_to_local_bounds(mesh_pose, box_min, box_max, local_query_min,
                                        local_query_max);
        stack[top++] = 0;
        while (top > 0) {
            int32_t node_index = stack[--top];
            const rt_physics_mesh_bvh_node *node;
            double node_min[3], node_max[3];
            if (node_index < 0 || node_index >= mesh->physics_bvh_node_count)
                continue;
            node = &nodes[node_index];
            node_min[0] = node->min[0];
            node_min[1] = node->min[1];
            node_min[2] = node->min[2];
            node_max[0] = node->max[0];
            node_max[1] = node->max[1];
            node_max[2] = node->max[2];
            if (!aabb_overlap_raw(node_min, node_max, local_query_min, local_query_max))
                continue;
            if (node->left >= 0 || node->right >= 0) {
                if (top + 2 > (int32_t)(sizeof(stack) / sizeof(stack[0]))) {
                    overflow = 1;
                    break;
                }
                if (node->right >= 0)
                    stack[top++] = node->right;
                if (node->left >= 0)
                    stack[top++] = node->left;
                continue;
            }
            for (int32_t item = node->start; item < node->start + node->count; ++item) {
                uint32_t tri = tri_indices[item];
                uint32_t i0 = mesh->indices[tri * 3u + 0u];
                uint32_t i1 = mesh->indices[tri * 3u + 1u];
                uint32_t i2 = mesh->indices[tri * 3u + 2u];
                double a[3], b[3], c[3], local[3], cur_normal[3], cur_depth;
                if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count ||
                    i2 >= mesh->vertex_count)
                    continue;
                local[0] = mesh->vertices[i0].pos[0];
                local[1] = mesh->vertices[i0].pos[1];
                local[2] = mesh->vertices[i0].pos[2];
                transform_point_from_pose(mesh_pose, local, a);
                local[0] = mesh->vertices[i1].pos[0];
                local[1] = mesh->vertices[i1].pos[1];
                local[2] = mesh->vertices[i1].pos[2];
                transform_point_from_pose(mesh_pose, local, b);
                local[0] = mesh->vertices[i2].pos[0];
                local[1] = mesh->vertices[i2].pos[1];
                local[2] = mesh->vertices[i2].pos[2];
                transform_point_from_pose(mesh_pose, local, c);
                if (test_triangle_box_obb(a, b, c, box, cur_normal, &cur_depth) &&
                    (!hit || cur_depth > best_depth)) {
                    best_depth = cur_depth;
                    vec3_copy(best_normal, cur_normal);
                    hit = 1;
                }
            }
        }
        if (!overflow)
            goto mesh_box_done;
        best_depth = 0.0;
        vec3_set(best_normal, 0.0, 1.0, 0.0);
        hit = 0;
    }
    for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
        uint32_t i0 = mesh->indices[i];
        uint32_t i1 = mesh->indices[i + 1];
        uint32_t i2 = mesh->indices[i + 2];
        double a[3], b[3], c[3], local[3], cur_normal[3], cur_depth;
        if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count || i2 >= mesh->vertex_count)
            continue;
        local[0] = mesh->vertices[i0].pos[0];
        local[1] = mesh->vertices[i0].pos[1];
        local[2] = mesh->vertices[i0].pos[2];
        transform_point_from_pose(mesh_pose, local, a);
        local[0] = mesh->vertices[i1].pos[0];
        local[1] = mesh->vertices[i1].pos[1];
        local[2] = mesh->vertices[i1].pos[2];
        transform_point_from_pose(mesh_pose, local, b);
        local[0] = mesh->vertices[i2].pos[0];
        local[1] = mesh->vertices[i2].pos[1];
        local[2] = mesh->vertices[i2].pos[2];
        transform_point_from_pose(mesh_pose, local, c);
        if (test_triangle_box_obb(a, b, c, box, cur_normal, &cur_depth) &&
            (!hit || cur_depth > best_depth)) {
            best_depth = cur_depth;
            vec3_copy(best_normal, cur_normal);
            hit = 1;
        }
    }
mesh_box_done:
    if (!hit)
        return 0;
    vec3_copy(normal, best_normal);
    *depth = best_depth;
    return 1;
}
