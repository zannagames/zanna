//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/physics/rt_physics3d_collision_meshconvex.inc  (#included from
// rt_physics3d_collision.c; not a standalone translation unit) Purpose: Physics3D GJK/EPA,
// mesh-like tests, and the collider-pair dispatcher
//===----------------------------------------------------------------------===//

/**
 * @file rt_physics3d_collision_meshconvex.inc
 * @brief Implements GJK/EPA, mesh and heightfield tests, compound traversal, and pair dispatch.
 *
 * The routines in this fragment provide bounded convex-overlap and penetration
 * recovery, triangle-versus-hull tests, terrain contacts, recursive compound
 * leaf selection, broadphase rejection, and top-level manifold construction
 * while preserving the body-A-to-body-B normal convention.
 */

typedef struct {
    double v[3];
} gjk_support_point;

typedef struct {
    gjk_support_point p[4];
    int32_t count;
} gjk_simplex;

typedef struct {
    int32_t a;
    int32_t b;
    int32_t c;
    double normal[3];
    double distance;
    int8_t active;
} epa_face;

typedef struct {
    int32_t a;
    int32_t b;
} epa_edge;

typedef void (*ph3d_gjk_support_fn)(void *ctx, const double *direction, gjk_support_point *out);

typedef struct {
    void *a_collider;
    const rt_collider_pose *a_pose;
    void *b_collider;
    const rt_collider_pose *b_pose;
} ph3d_collider_pair_support_ctx;

typedef struct {
    double tri[3][3];
    void *hull_collider;
    const rt_collider_pose *hull_pose;
} ph3d_triangle_hull_support_ctx;

/// @brief Triple cross product out = (a × b) × c, with a stable fallback when it degenerates.
/// @details GJK uses this to get a direction lying in an edge's plane and pointing toward the
/// origin;
///          when the inputs are collinear (near-zero result) it substitutes an arbitrary
///          perpendicular.
/// @param a First three-component vector.
/// @param b Second three-component vector.
/// @param c Third three-component vector.
/// @param out Three-component output receiving the stable triple cross product.
static void vec3_triple_cross(const double *a, const double *b, const double *c, double *out) {
    double tmp[3];
    vec3_cross(a, b, tmp);
    vec3_cross(tmp, c, out);
    if (vec3_len_sq(out) <= 1e-18) {
        double fallback[3] = {fabs(a[0]) < 0.9 ? 1.0 : 0.0, fabs(a[0]) < 0.9 ? 0.0 : 1.0, 0.0};
        vec3_cross(a, fallback, out);
        if (vec3_normalize_in_place(out) <= 1e-12)
            vec3_set(out, 0.0, 1.0, 0.0);
    }
}

/// @brief Minkowski-difference support point: support(A, dir) − support(B, −dir).
/// @details The farthest point of the configuration-space obstacle (A ⊖ B) along @p direction —
///          the single primitive GJK and EPA query to probe the shapes' combined geometry.
/// @param a_collider First convex Collider3D.
/// @param a_pose World pose of @p a_collider.
/// @param b_collider Second convex Collider3D.
/// @param b_pose World pose of @p b_collider.
/// @param direction Three-component Minkowski search direction.
/// @param out Output receiving the Minkowski-difference support point.
static void gjk_support(void *a_collider,
                        const rt_collider_pose *a_pose,
                        void *b_collider,
                        const rt_collider_pose *b_pose,
                        const double *direction,
                        gjk_support_point *out) {
    double pa[3];
    double pb[3];
    double inv_dir[3];
    vec3_negate(direction, inv_dir);
    collider_support_point(a_collider, a_pose, direction, pa);
    collider_support_point(b_collider, b_pose, inv_dir, pb);
    vec3_sub(pa, pb, out->v);
}

/// @brief GJK support callback for a collider pair: the Minkowski-difference support point
///   (collider A's support along @p direction minus collider B's along the opposite direction).
/// @param ctx Collider-pair support context.
/// @param direction Three-component Minkowski search direction.
/// @param out Output receiving the support point.
static void gjk_support_collider_pair(void *ctx, const double *direction, gjk_support_point *out) {
    ph3d_collider_pair_support_ctx *pair = (ph3d_collider_pair_support_ctx *)ctx;
    if (!pair || !out)
        return;
    gjk_support(pair->a_collider, pair->a_pose, pair->b_collider, pair->b_pose, direction, out);
}

/// @brief Support point of a triangle: the vertex farthest along @p direction.
/// @param tri Three world-space triangle vertices.
/// @param direction Three-component support direction.
/// @param out Three-component output receiving the farthest vertex.
static void triangle_support_point(const double tri[3][3], const double *direction, double *out) {
    double best_dot = -DBL_MAX;
    int best = 0;
    for (int i = 0; i < 3; ++i) {
        double d = vec3_dot(tri[i], direction);
        if (d > best_dot) {
            best_dot = d;
            best = i;
        }
    }
    vec3_copy(out, tri[best]);
}

/// @brief GJK support callback for a triangle-vs-convex-hull pair: the Minkowski-difference
///   support point (triangle support along @p direction minus hull support along −direction).
/// @param ctx Triangle/hull support context.
/// @param direction Three-component Minkowski search direction.
/// @param out Output receiving the support point.
static void gjk_support_triangle_hull(void *ctx, const double *direction, gjk_support_point *out) {
    ph3d_triangle_hull_support_ctx *pair = (ph3d_triangle_hull_support_ctx *)ctx;
    double tri_point[3];
    double hull_point[3];
    double inv_dir[3];
    if (!pair || !direction || !out)
        return;
    vec3_negate(direction, inv_dir);
    triangle_support_point((const double (*)[3])pair->tri, direction, tri_point);
    collider_support_point(pair->hull_collider, pair->hull_pose, inv_dir, hull_point);
    vec3_sub(tri_point, hull_point, out->v);
}

/// @brief Reduce the simplex to the single vertex @p a.
/// @param simplex GJK simplex to replace.
/// @param a Most-recent support vertex retained.
static void gjk_simplex_set1(gjk_simplex *simplex, const gjk_support_point *a) {
    simplex->p[0] = *a;
    simplex->count = 1;
}

/// @brief Set the simplex to the line segment (b, a) with @p a as the most-recent vertex.
/// @param simplex GJK simplex to replace.
/// @param b Older segment support vertex.
/// @param a Most-recent segment support vertex.
static void gjk_simplex_set2(gjk_simplex *simplex,
                             const gjk_support_point *b,
                             const gjk_support_point *a) {
    simplex->p[0] = *b;
    simplex->p[1] = *a;
    simplex->count = 2;
}

/// @brief Set the simplex to the triangle (c, b, a) with @p a as the most-recent vertex.
/// @param simplex GJK simplex to replace.
/// @param c Oldest triangle support vertex.
/// @param b Middle triangle support vertex.
/// @param a Most-recent triangle support vertex.
static void gjk_simplex_set3(gjk_simplex *simplex,
                             const gjk_support_point *c,
                             const gjk_support_point *b,
                             const gjk_support_point *a) {
    simplex->p[0] = *c;
    simplex->p[1] = *b;
    simplex->p[2] = *a;
    simplex->count = 3;
}

/// @brief Evolve a 2-point (line) simplex toward the origin, updating the search @p direction.
/// @details Keeps the edge if the origin projects onto it (search perpendicular toward origin),
///          else drops back to the newest vertex. Returns 0 (origin not yet enclosed).
/// @param simplex Two-point GJK simplex updated in place.
/// @param direction Three-component output receiving the next search direction.
/// @return Zero because a line simplex cannot enclose the origin in 3D.
static int gjk_update_line(gjk_simplex *simplex, double *direction) {
    gjk_support_point a = simplex->p[simplex->count - 1];
    gjk_support_point b = simplex->p[simplex->count - 2];
    double ao[3];
    double ab[3];
    vec3_negate(a.v, ao);
    vec3_sub(b.v, a.v, ab);
    if (vec3_dot(ab, ao) > 0.0) {
        vec3_triple_cross(ab, ao, ab, direction);
    } else {
        gjk_simplex_set1(simplex, &a);
        vec3_copy(direction, ao);
    }
    return 0;
}

/// @brief Evolve a 3-point (triangle) simplex toward the origin, updating the search @p direction.
/// @details Tests the origin against the triangle's edge regions and the two face half-spaces,
///          reducing to the relevant edge or keeping the triangle (with the face normal toward the
///          origin) as the next search direction. Returns 0 (origin not yet enclosed).
/// @param simplex Three-point GJK simplex updated in place.
/// @param direction Three-component output receiving the next search direction.
/// @return Zero because a triangle simplex cannot enclose a 3D volume.
static int gjk_update_triangle(gjk_simplex *simplex, double *direction) {
    gjk_support_point a = simplex->p[2];
    gjk_support_point b = simplex->p[1];
    gjk_support_point c = simplex->p[0];
    double ao[3];
    double ab[3];
    double ac[3];
    double abc[3];
    double edge_perp[3];

    vec3_negate(a.v, ao);
    vec3_sub(b.v, a.v, ab);
    vec3_sub(c.v, a.v, ac);
    vec3_cross(ab, ac, abc);

    vec3_cross(abc, ac, edge_perp);
    if (vec3_dot(edge_perp, ao) > 0.0) {
        if (vec3_dot(ac, ao) > 0.0) {
            gjk_simplex_set2(simplex, &c, &a);
            vec3_triple_cross(ac, ao, ac, direction);
        } else {
            gjk_simplex_set2(simplex, &b, &a);
            return gjk_update_line(simplex, direction);
        }
        return 0;
    }

    vec3_cross(ab, abc, edge_perp);
    if (vec3_dot(edge_perp, ao) > 0.0) {
        gjk_simplex_set2(simplex, &b, &a);
        return gjk_update_line(simplex, direction);
    }

    if (vec3_dot(abc, ao) > 0.0) {
        vec3_copy(direction, abc);
    } else {
        gjk_simplex_set3(simplex, &b, &c, &a);
        vec3_negate(abc, direction);
    }
    return 0;
}

/// @brief Compute the unit normal of face (a, b, c) oriented to point away from @p opposite.
/// @details Used per-face in the tetrahedron test so each face normal points outward; degenerate
///          faces fall back to +Y.
/// @param a First face support vertex.
/// @param b Second face support vertex.
/// @param c Third face support vertex.
/// @param opposite Tetrahedron vertex opposite the face.
/// @param normal Three-component output receiving the outward unit normal.
static void gjk_face_normal_away_from_point(const gjk_support_point *a,
                                            const gjk_support_point *b,
                                            const gjk_support_point *c,
                                            const gjk_support_point *opposite,
                                            double *normal) {
    double ab[3];
    double ac[3];
    double ao[3];
    vec3_sub(b->v, a->v, ab);
    vec3_sub(c->v, a->v, ac);
    vec3_cross(ab, ac, normal);
    if (vec3_normalize_in_place(normal) <= 1e-12) {
        vec3_set(normal, 0.0, 1.0, 0.0);
        return;
    }
    vec3_sub(opposite->v, a->v, ao);
    if (vec3_dot(normal, ao) > 0.0)
        vec3_negate(normal, normal);
}

/// @brief Evolve a 4-point (tetrahedron) simplex; report whether it encloses the origin.
/// @details Checks the origin against each of the three faces touching the newest vertex; if it
/// lies
///          outside one, drops to that face and continues. If inside all three, the origin is
///          enclosed.
/// @param simplex Four-point GJK simplex updated when the origin lies outside a face.
/// @param direction Three-component output receiving the next search direction when needed.
/// @return 1 if the origin is inside the tetrahedron (collision), 0 to keep searching.
static int gjk_update_tetrahedron(gjk_simplex *simplex, double *direction) {
    gjk_support_point a = simplex->p[3];
    gjk_support_point b = simplex->p[2];
    gjk_support_point c = simplex->p[1];
    gjk_support_point d = simplex->p[0];
    double ao[3];
    double normal[3];
    vec3_negate(a.v, ao);

    gjk_face_normal_away_from_point(&a, &b, &c, &d, normal);
    if (vec3_dot(normal, ao) > 0.0) {
        gjk_simplex_set3(simplex, &c, &b, &a);
        vec3_copy(direction, normal);
        return 0;
    }

    gjk_face_normal_away_from_point(&a, &c, &d, &b, normal);
    if (vec3_dot(normal, ao) > 0.0) {
        gjk_simplex_set3(simplex, &d, &c, &a);
        vec3_copy(direction, normal);
        return 0;
    }

    gjk_face_normal_away_from_point(&a, &d, &b, &c, normal);
    if (vec3_dot(normal, ao) > 0.0) {
        gjk_simplex_set3(simplex, &b, &d, &a);
        vec3_copy(direction, normal);
        return 0;
    }

    return 1;
}

/// @brief Advance the GJK simplex one step by dispatching on its current vertex count.
/// @param simplex GJK simplex to classify and reduce.
/// @param direction Three-component output receiving the next search direction.
/// @return 1 when the simplex (a tetrahedron) encloses the origin, else 0.
static int gjk_update_simplex(gjk_simplex *simplex, double *direction) {
    if (!simplex || !direction)
        return 0;
    if (simplex->count == 2)
        return gjk_update_line(simplex, direction);
    if (simplex->count == 3)
        return gjk_update_triangle(simplex, direction);
    if (simplex->count == 4)
        return gjk_update_tetrahedron(simplex, direction);
    if (simplex->count == 1)
        vec3_negate(simplex->p[0].v, direction);
    return 0;
}

/// @brief Build an EPA polytope face from three vertex indices, oriented outward from the origin.
/// @details Computes the unit normal and its signed distance from the origin; flips the winding so
///          the normal faces away from the origin (distance >= 0). Returns 0 for a degenerate face.
/// @param vertices EPA support-vertex array.
/// @param a First face vertex index.
/// @param b Second face vertex index.
/// @param c Third face vertex index.
/// @param face Output receiving the oriented EPA face.
/// @return One for a valid non-degenerate face, otherwise zero.
static int epa_make_face(
    const gjk_support_point *vertices, int32_t a, int32_t b, int32_t c, epa_face *face) {
    double ab[3];
    double ac[3];
    if (!vertices || !face)
        return 0;
    face->a = a;
    face->b = b;
    face->c = c;
    face->active = 1;
    vec3_sub(vertices[b].v, vertices[a].v, ab);
    vec3_sub(vertices[c].v, vertices[a].v, ac);
    vec3_cross(ab, ac, face->normal);
    if (vec3_normalize_in_place(face->normal) <= 1e-12)
        return 0;
    face->distance = vec3_dot(face->normal, vertices[a].v);
    if (face->distance < 0.0) {
        int32_t tmp = face->b;
        face->b = face->c;
        face->c = tmp;
        vec3_negate(face->normal, face->normal);
        face->distance = -face->distance;
    }
    return 1;
}

/// @brief Add edge (a, b) to the EPA horizon, cancelling it against an existing reverse edge.
/// @details A directed edge shared by two removed faces appears once as (a,b) and once as (b,a);
///          cancelling those pairs leaves exactly the silhouette/horizon edges that the new faces
///          must close over. Caps the edge list to avoid unbounded growth.
/// @param edges Fixed-capacity EPA horizon-edge array.
/// @param edge_count In/out number of active edges.
/// @param a Directed edge start vertex index.
/// @param b Directed edge end vertex index.
/// @return One when the edge was canceled or appended, otherwise zero on capacity exhaustion.
static int epa_add_edge(epa_edge *edges, int32_t *edge_count, int32_t a, int32_t b) {
    if (!edges || !edge_count)
        return 0;
    for (int32_t i = 0; i < *edge_count; ++i) {
        if (edges[i].a == b && edges[i].b == a) {
            edges[i] = edges[--(*edge_count)];
            return 1;
        }
    }
    if (*edge_count >= 192)
        return 0;
    edges[*edge_count].a = a;
    edges[*edge_count].b = b;
    (*edge_count)++;
    return 1;
}

/// @brief Append an EPA face (a, b, c) to the polytope, skipping degenerate faces.
/// @param vertices EPA support-vertex array.
/// @param faces Fixed-capacity face array.
/// @param face_count In/out number of active face records.
/// @param a First face vertex index.
/// @param b Second face vertex index.
/// @param c Third face vertex index.
/// @return 0 only when the face array is full; 1 otherwise (including a skipped degenerate face).
static int epa_add_face(const gjk_support_point *vertices,
                        epa_face *faces,
                        int32_t *face_count,
                        int32_t a,
                        int32_t b,
                        int32_t c) {
    if (!faces || !face_count || *face_count >= 128)
        return 0;
    if (!epa_make_face(vertices, a, b, c, &faces[*face_count]))
        return 1;
    (*face_count)++;
    return 1;
}

/// @brief Run EPA on GJK's enclosing tetrahedron to find the penetration normal and depth.
/// @details Iteratively expands the polytope toward the Minkowski surface: each step takes a
/// support
///          point past the closest face, removes all faces it can "see", and re-closes the
///          resulting horizon, until the closest face stops moving (within @p tolerance). Writes
///          the outward contact @p normal and penetration @p depth.
/// @param support_fn Shape-pair support callback.
/// @param support_ctx Opaque context passed to @p support_fn.
/// @param simplex Origin-enclosing GJK tetrahedron.
/// @param normal Three-component output receiving the EPA normal.
/// @param depth Output receiving non-negative penetration depth.
/// @return 1 on success, 0 if the input simplex is not a tetrahedron or the polytope degenerates.
static int epa_solve_with_support(ph3d_gjk_support_fn support_fn,
                                  void *support_ctx,
                                  const gjk_simplex *simplex,
                                  double *normal,
                                  double *depth) {
    gjk_support_point vertices[64];
    epa_face faces[128];
    int32_t vertex_count = 4;
    int32_t face_count = 0;
    const double tolerance = 1e-6;
    if (!support_fn || !simplex || simplex->count < 4 || !normal || !depth)
        return 0;
    for (int32_t i = 0; i < 4; ++i)
        vertices[i] = simplex->p[i];
    if (!epa_add_face(vertices, faces, &face_count, 0, 1, 2) ||
        !epa_add_face(vertices, faces, &face_count, 0, 3, 1) ||
        !epa_add_face(vertices, faces, &face_count, 0, 2, 3) ||
        !epa_add_face(vertices, faces, &face_count, 1, 3, 2))
        return 0;

    for (int32_t iter = 0; iter < 48; ++iter) {
        int32_t best = -1;
        double best_distance = DBL_MAX;
        for (int32_t i = 0; i < face_count; ++i) {
            if (faces[i].active && faces[i].distance < best_distance) {
                best_distance = faces[i].distance;
                best = i;
            }
        }
        if (best < 0)
            return 0;

        gjk_support_point support;
        support_fn(support_ctx, faces[best].normal, &support);
        double support_distance = vec3_dot(support.v, faces[best].normal);
        if (support_distance - faces[best].distance <= tolerance) {
            vec3_copy(normal, faces[best].normal);
            if (vec3_normalize_in_place(normal) <= 1e-12)
                vec3_set(normal, 0.0, 1.0, 0.0);
            *depth = faces[best].distance;
            if (*depth < 0.0)
                *depth = 0.0;
            return 1;
        }

        if (vertex_count >= 64)
            break;
        int32_t new_vertex = vertex_count++;
        vertices[new_vertex] = support;

        epa_edge edges[192];
        int32_t edge_count = 0;
        for (int32_t i = 0; i < face_count; ++i) {
            double to_support[3];
            if (!faces[i].active)
                continue;
            vec3_sub(support.v, vertices[faces[i].a].v, to_support);
            if (vec3_dot(faces[i].normal, to_support) > tolerance) {
                faces[i].active = 0;
                if (!epa_add_edge(edges, &edge_count, faces[i].a, faces[i].b) ||
                    !epa_add_edge(edges, &edge_count, faces[i].b, faces[i].c) ||
                    !epa_add_edge(edges, &edge_count, faces[i].c, faces[i].a))
                    return 0;
            }
        }
        for (int32_t i = 0; i < edge_count; ++i) {
            if (!epa_add_face(vertices, faces, &face_count, edges[i].a, edges[i].b, new_vertex))
                return 0;
        }
    }
    return 0;
}

/// @brief Run EPA (Expanding Polytope Algorithm) on a collider pair, given a GJK simplex that
///   encloses the origin, to recover the penetration @p normal and @p depth.
/// @param a_collider First convex Collider3D.
/// @param a_pose World pose of @p a_collider.
/// @param b_collider Second convex Collider3D.
/// @param b_pose World pose of @p b_collider.
/// @param simplex Origin-enclosing GJK tetrahedron.
/// @param normal Three-component output receiving the penetration normal.
/// @param depth Output receiving non-negative penetration depth.
/// @return 1 on success, 0 if EPA fails to converge.
static int epa_solve(void *a_collider,
                     const rt_collider_pose *a_pose,
                     void *b_collider,
                     const rt_collider_pose *b_pose,
                     const gjk_simplex *simplex,
                     double *normal,
                     double *depth) {
    ph3d_collider_pair_support_ctx ctx = {a_collider, a_pose, b_collider, b_pose};
    return epa_solve_with_support(gjk_support_collider_pair, &ctx, simplex, normal, depth);
}

/// @brief Generic GJK overlap test followed by EPA penetration recovery, driven by a
///   caller-provided @p support_fn and seeded from @p seed_direction.
/// @param support_fn Shape-pair support callback.
/// @param support_ctx Opaque context passed to @p support_fn.
/// @param seed_direction Optional initial search direction.
/// @param normal Three-component output receiving the overlap normal.
/// @param depth Output receiving non-negative penetration depth.
/// @return 1 if the shapes overlap (with @p normal / @p depth set), 0 otherwise.
static int gjk_epa_with_support(ph3d_gjk_support_fn support_fn,
                                void *support_ctx,
                                const double *seed_direction,
                                double *normal,
                                double *depth) {
    gjk_simplex simplex = {0};
    double direction[3];
    gjk_support_point support;
    if (!support_fn || !normal || !depth)
        return 0;
    if (seed_direction)
        vec3_copy(direction, seed_direction);
    else
        vec3_set(direction, 1.0, 0.0, 0.0);
    if (vec3_normalize_in_place(direction) <= 1e-12)
        vec3_set(direction, 1.0, 0.0, 0.0);

    support_fn(support_ctx, direction, &support);
    gjk_simplex_set1(&simplex, &support);
    vec3_negate(support.v, direction);
    if (vec3_normalize_in_place(direction) <= 1e-12)
        vec3_set(direction, 0.0, 1.0, 0.0);

    for (int32_t iter = 0; iter < 32; ++iter) {
        support_fn(support_ctx, direction, &support);
        if (vec3_dot(support.v, direction) < 1e-9)
            return 0;
        if (simplex.count >= 4)
            return 0;
        simplex.p[simplex.count++] = support;
        if (gjk_update_simplex(&simplex, direction)) {
            if (epa_solve_with_support(support_fn, support_ctx, &simplex, normal, depth))
                return 1;
            /* GJK enclosed the origin (shapes overlap) but EPA could not expand a valid
             * polytope (degenerate simplex, or the 64-vertex cap in epa_solve_with_support).
             * Report the overlap with zero penetration depth and the best available normal
             * (the caller's seed direction, else +Y) rather than no-contact: a 0-depth
             * contact still enforces the velocity-level non-penetration constraint in the
             * solver (the shapes stop approaching) while contributing no Baumgarte push-out,
             * which avoids both tunneling and an explosive correction from a fabricated
             * depth. Persistent counts of this fallback (Diagnostics3D.EpaFallbacks)
             * usually mean an over-detailed hull collider that should be decimated. */
            rt_game3d_diag_record_epa_fallback();
            if (seed_direction) {
                vec3_copy(normal, seed_direction);
                if (vec3_normalize_in_place(normal) <= 1e-12)
                    vec3_set(normal, 0.0, 1.0, 0.0);
            } else {
                vec3_set(normal, 0.0, 1.0, 0.0);
            }
            *depth = 0.0;
            return 1;
        }
        if (vec3_normalize_in_place(direction) <= 1e-12)
            vec3_set(direction, 1.0, 0.0, 0.0);
    }
    return 0;
}

/// @brief Test two convex colliders for overlap and, when overlapping, report the contact via EPA.
/// @details Runs GJK seeded from the center-to-center direction until it builds an origin-enclosing
///          tetrahedron (or an iteration cap proves separation), then hands that simplex to EPA for
///          the penetration @p normal and @p depth.
/// @param a_collider First convex Collider3D.
/// @param a_pose World pose of @p a_collider.
/// @param b_collider Second convex Collider3D.
/// @param b_pose World pose of @p b_collider.
/// @param a_center Optional first shape center used to seed and orient the normal.
/// @param b_center Optional second shape center used to seed and orient the normal.
/// @param normal Three-component output receiving the A-to-B contact normal.
/// @param depth Output receiving non-negative penetration depth.
/// @return 1 if the shapes overlap (normal/depth written), 0 if separate.
static int test_convex_hull_gjk_epa(void *a_collider,
                                    const rt_collider_pose *a_pose,
                                    void *b_collider,
                                    const rt_collider_pose *b_pose,
                                    const double *a_center,
                                    const double *b_center,
                                    double *normal,
                                    double *depth) {
    gjk_simplex simplex = {0};
    double direction[3];
    gjk_support_point support;
    if (!a_collider || !a_pose || !b_collider || !b_pose || !normal || !depth)
        return 0;
    if (a_center && b_center)
        vec3_sub(b_center, a_center, direction);
    else
        vec3_set(direction, 1.0, 0.0, 0.0);
    if (vec3_normalize_in_place(direction) <= 1e-12)
        vec3_set(direction, 1.0, 0.0, 0.0);

    gjk_support(a_collider, a_pose, b_collider, b_pose, direction, &support);
    gjk_simplex_set1(&simplex, &support);
    vec3_negate(support.v, direction);
    if (vec3_normalize_in_place(direction) <= 1e-12)
        vec3_set(direction, 0.0, 1.0, 0.0);

    for (int32_t iter = 0; iter < 32; ++iter) {
        gjk_support(a_collider, a_pose, b_collider, b_pose, direction, &support);
        if (vec3_dot(support.v, direction) < 1e-9)
            return 0;
        if (simplex.count >= 4)
            return 0;
        simplex.p[simplex.count++] = support;
        if (gjk_update_simplex(&simplex, direction)) {
            if (epa_solve(a_collider, a_pose, b_collider, b_pose, &simplex, normal, depth)) {
                if (a_center && b_center) {
                    double center_delta[3];
                    vec3_sub(b_center, a_center, center_delta);
                    if (vec3_dot(normal, center_delta) < 0.0)
                        vec3_negate(normal, normal);
                }
                return 1;
            }
            rt_game3d_diag_record_epa_fallback();
            if (a_center && b_center) {
                vec3_sub(b_center, a_center, normal);
                if (vec3_normalize_in_place(normal) <= 1e-12)
                    vec3_set(normal, 0.0, 1.0, 0.0);
            } else {
                vec3_set(normal, 0.0, 1.0, 0.0);
            }
            *depth = 0.0;
            return 1;
        }
        if (vec3_normalize_in_place(direction) <= 1e-12)
            vec3_set(direction, 1.0, 0.0, 0.0);
    }
    return 0;
}

/// @brief Narrow-phase collision test between a triangle and a convex-hull collider via
///   GJK/EPA, seeded from the triangle centroid toward the hull.
/// @param tri Three world-space triangle vertices.
/// @param hull_collider Convex-hull Collider3D.
/// @param hull_pose World pose of @p hull_collider.
/// @param hull_center Optional hull center used to seed and orient the normal.
/// @param normal Three-component output receiving the triangle-to-hull normal.
/// @param depth Output receiving non-negative penetration depth.
/// @return 1 on overlap (filling @p normal / @p depth), 0 otherwise.
static int test_triangle_convex_hull_gjk_epa(const double tri[3][3],
                                             void *hull_collider,
                                             const rt_collider_pose *hull_pose,
                                             const double *hull_center,
                                             double *normal,
                                             double *depth) {
    if (!tri || !hull_collider || !hull_pose || !normal || !depth)
        return 0;
    ph3d_triangle_hull_support_ctx ctx;
    double centroid[3] = {(tri[0][0] + tri[1][0] + tri[2][0]) / 3.0,
                          (tri[0][1] + tri[1][1] + tri[2][1]) / 3.0,
                          (tri[0][2] + tri[1][2] + tri[2][2]) / 3.0};
    double seed[3];
    memcpy(ctx.tri, tri, sizeof(ctx.tri));
    ctx.hull_collider = hull_collider;
    ctx.hull_pose = hull_pose;
    if (hull_center)
        vec3_sub(hull_center, centroid, seed);
    else
        vec3_set(seed, 0.0, 1.0, 0.0);
    if (!gjk_epa_with_support(gjk_support_triangle_hull, &ctx, seed, normal, depth))
        return 0;
    if (hull_center) {
        double delta[3];
        vec3_sub(hull_center, centroid, delta);
        if (vec3_dot(normal, delta) < 0.0)
            vec3_negate(normal, normal);
    }
    return 1;
}

/// @brief Mesh-vs-convex-hull narrow phase: broad mesh nodes by the hull AABB,
///   then use GJK/EPA against each candidate triangle.
/// @param mesh Triangle mesh providing candidate faces and optional BVH.
/// @param mesh_pose World pose applied to @p mesh.
/// @param hull_collider Convex-hull Collider3D.
/// @param hull_pose World pose of @p hull_collider.
/// @param normal Three-component output receiving the mesh-to-hull normal.
/// @param depth Output receiving the deepest penetration.
/// @return One when any mesh triangle overlaps the hull, otherwise zero.
static int test_meshlike_convex_hull(rt_mesh3d *mesh,
                                     const rt_collider_pose *mesh_pose,
                                     void *hull_collider,
                                     const rt_collider_pose *hull_pose,
                                     double *normal,
                                     double *depth) {
    double hull_min[3], hull_max[3];
    double hull_center[3];
    double best_depth = 0.0;
    double best_normal[3] = {0.0, 1.0, 0.0};
    int hit = 0;
    if (!mesh || !mesh_pose || !hull_collider || !hull_pose)
        return 0;
    rt_collider3d_compute_world_aabb_raw(hull_collider,
                                         hull_pose->position,
                                         hull_pose->rotation,
                                         hull_pose->scale,
                                         hull_min,
                                         hull_max);
    hull_center[0] = (hull_min[0] + hull_max[0]) * 0.5;
    hull_center[1] = (hull_min[1] + hull_max[1]) * 0.5;
    hull_center[2] = (hull_min[2] + hull_max[2]) * 0.5;
    rt_mesh3d_refresh_bounds(mesh);
    if (mesh_physics_bvh_rebuild(mesh)) {
        const rt_physics_mesh_bvh_node *nodes =
            (const rt_physics_mesh_bvh_node *)mesh->physics_bvh_nodes;
        const uint32_t *tri_indices = mesh->physics_bvh_tri_indices;
        int32_t *stack = NULL;
        int32_t top = 0;
        int32_t stack_capacity = 0;
        int overflow = 0;
        double local_hull_min[3], local_hull_max[3];
        /* Traverse in mesh-local space: convert the hull AABB once instead of
         * transforming every visited node's 8 corners to world (same trick the
         * ray path uses). */
        ph3d_world_aabb_to_local_bounds(mesh_pose, hull_min, hull_max, local_hull_min,
                                        local_hull_max);
        if (!ph3d_i32_stack_push(&stack, &top, &stack_capacity, 0))
            overflow = 1;
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
            if (!aabb_overlap_raw(node_min, node_max, local_hull_min, local_hull_max))
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
                uint32_t tri_index = tri_indices[item];
                uint32_t i0 = mesh->indices[tri_index * 3u + 0u];
                uint32_t i1 = mesh->indices[tri_index * 3u + 1u];
                uint32_t i2 = mesh->indices[tri_index * 3u + 2u];
                double tri[3][3];
                double local[3];
                double cur_normal[3];
                double cur_depth;
                if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count ||
                    i2 >= mesh->vertex_count)
                    continue;
                local[0] = mesh->vertices[i0].pos[0];
                local[1] = mesh->vertices[i0].pos[1];
                local[2] = mesh->vertices[i0].pos[2];
                transform_point_from_pose(mesh_pose, local, tri[0]);
                local[0] = mesh->vertices[i1].pos[0];
                local[1] = mesh->vertices[i1].pos[1];
                local[2] = mesh->vertices[i1].pos[2];
                transform_point_from_pose(mesh_pose, local, tri[1]);
                local[0] = mesh->vertices[i2].pos[0];
                local[1] = mesh->vertices[i2].pos[1];
                local[2] = mesh->vertices[i2].pos[2];
                transform_point_from_pose(mesh_pose, local, tri[2]);
                if (test_triangle_convex_hull_gjk_epa((const double (*)[3])tri,
                                                      hull_collider,
                                                      hull_pose,
                                                      hull_center,
                                                      cur_normal,
                                                      &cur_depth) &&
                    (!hit || cur_depth > best_depth)) {
                    best_depth = cur_depth;
                    vec3_copy(best_normal, cur_normal);
                    hit = 1;
                }
            }
        }
        if (!overflow)
            goto mesh_hull_done;
        best_depth = 0.0;
        vec3_set(best_normal, 0.0, 1.0, 0.0);
        hit = 0;
    }

    for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
        uint32_t i0 = mesh->indices[i];
        uint32_t i1 = mesh->indices[i + 1u];
        uint32_t i2 = mesh->indices[i + 2u];
        double tri[3][3];
        double local[3];
        double cur_normal[3];
        double cur_depth;
        if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count || i2 >= mesh->vertex_count)
            continue;
        local[0] = mesh->vertices[i0].pos[0];
        local[1] = mesh->vertices[i0].pos[1];
        local[2] = mesh->vertices[i0].pos[2];
        transform_point_from_pose(mesh_pose, local, tri[0]);
        local[0] = mesh->vertices[i1].pos[0];
        local[1] = mesh->vertices[i1].pos[1];
        local[2] = mesh->vertices[i1].pos[2];
        transform_point_from_pose(mesh_pose, local, tri[1]);
        local[0] = mesh->vertices[i2].pos[0];
        local[1] = mesh->vertices[i2].pos[1];
        local[2] = mesh->vertices[i2].pos[2];
        transform_point_from_pose(mesh_pose, local, tri[2]);
        if (test_triangle_convex_hull_gjk_epa((const double (*)[3])tri,
                                              hull_collider,
                                              hull_pose,
                                              hull_center,
                                              cur_normal,
                                              &cur_depth) &&
            (!hit || cur_depth > best_depth)) {
            best_depth = cur_depth;
            vec3_copy(best_normal, cur_normal);
            hit = 1;
        }
    }

mesh_hull_done:
    if (!hit)
        return 0;
    vec3_copy(normal, best_normal);
    *depth = best_depth;
    return 1;
}

/// @brief Sphere-vs-heightfield narrow phase.
///
/// Transforms the sphere center into local heightfield space, samples
/// the field at (x, z) to get surface height + local normal, and tests
/// whether the sphere bottom is below the surface. Skips the cost of
/// per-cell triangle intersection by using the heightfield's analytical
/// normal directly.
/// @param heightfield Heightfield Collider3D handle to sample.
/// @param field_pose World pose of the heightfield.
/// @param sphere Sphere proxy body to test.
/// @param normal Three-component output receiving the field-to-sphere normal.
/// @param depth Output receiving normal-projected penetration depth.
/// @return One when the sphere penetrates the sampled surface, otherwise zero.
static int test_heightfield_sphere(void *heightfield,
                                   const rt_collider_pose *field_pose,
                                   const rt_body3d *sphere,
                                   double *normal,
                                   double *depth) {
    double local_center[3];
    double surface_height = 0.0;
    double local_normal[3] = {0.0, 1.0, 0.0};
    double scale_y;
    double local_radius_y;
    double penetration;
    transform_point_to_local(field_pose, sphere->position, local_center);
    if (!rt_collider3d_sample_heightfield_raw(
            heightfield, local_center[0], local_center[2], &surface_height, local_normal))
        return 0;
    scale_y = pose_abs_scale_or_unit(field_pose->scale[1]);
    local_radius_y = sphere->radius / scale_y;
    penetration = surface_height - (local_center[1] - local_radius_y);
    if (penetration <= 0.0)
        return 0;
    transform_normal_from_local(field_pose, local_normal, normal);
    /* Penetration was measured vertically; project onto the surface normal (the
     * solver's push direction) so steep slopes do not eject with 1/cos inflation. */
    *depth = penetration * clampd(local_normal[1], 0.0, 1.0) * scale_y;
    if (!isfinite(*depth) || *depth <= 0.0) {
        /* A malformed heightfield sample can yield a non-finite or absurd surface
         * height; reject rather than hand the solver an explosive positional
         * correction. */
        return 0;
    }
    return 1;
}

/// @brief Capsule-vs-heightfield narrow phase.
///
/// Samples along the oriented capsule axis and keeps the deepest hit. The sample
/// count adapts to the axis length (like the box-capsule path) so a long capsule
/// lying across a narrow ridge cannot straddle it between fixed samples.
/// @param heightfield Heightfield Collider3D handle to sample.
/// @param field_pose World pose of the heightfield.
/// @param capsule Capsule proxy body to test.
/// @param normal Three-component output receiving the deepest field normal.
/// @param depth Output receiving the deepest penetration.
/// @return One when any capsule-axis sphere sample penetrates, otherwise zero.
static int test_heightfield_capsule(void *heightfield,
                                    const rt_collider_pose *field_pose,
                                    const rt_body3d *capsule,
                                    double *normal,
                                    double *depth) {
    double half_axis = fmax(capsule->height * 0.5 - capsule->radius, 0.0);
    double axis_a[3], axis_b[3];
    double best_depth = 0.0;
    double best_normal[3] = {0.0, 1.0, 0.0};
    int samples = capsule_axis_sample_count(half_axis * 2.0, capsule->radius);
    capsule_axis_endpoints(capsule, axis_a, axis_b);
    for (int i = 0; i < samples; ++i) {
        double t = samples == 1 ? 0.5 : (double)i / (double)(samples - 1);
        rt_body3d sphere = {0};
        double cur_normal[3];
        double cur_depth;
        sphere.shape = PH3D_SHAPE_SPHERE;
        sphere.position[0] = axis_a[0] + (axis_b[0] - axis_a[0]) * t;
        sphere.position[1] = axis_a[1] + (axis_b[1] - axis_a[1]) * t;
        sphere.position[2] = axis_a[2] + (axis_b[2] - axis_a[2]) * t;
        sphere.radius = capsule->radius;
        if (test_heightfield_sphere(heightfield, field_pose, &sphere, cur_normal, &cur_depth) &&
            cur_depth > best_depth) {
            best_depth = cur_depth;
            vec3_copy(best_normal, cur_normal);
        }
    }
    if (best_depth <= 0.0)
        return 0;
    *depth = best_depth;
    vec3_copy(normal, best_normal);
    return 1;
}

/// @brief Choose the box's local face whose outward world normal points most downward.
/// @details A tumbled box's local -Y face can be sideways or on top; probing it lets the
///   box sink into the heightfield before any contact fires. Selecting the face by its
///   world orientation keeps the 3x3 probe on the true lowest face for any rotation.
/// @param box_pose World pose whose rotation determines the lowest face.
/// @param axis_out Local axis index (0..2) of the chosen face.
/// @param sign_out +1/-1 selecting the positive/negative face along that axis.
static void heightfield_box_lowest_face(const rt_collider_pose *box_pose,
                                        int *axis_out,
                                        double *sign_out) {
    double best_y = DBL_MAX;
    int best_axis = 1;
    double best_sign = -1.0;
    for (int axis = 0; axis < 3; ++axis) {
        double local_axis[3] = {0.0, 0.0, 0.0};
        double world_axis[3];
        local_axis[axis] = 1.0;
        quat_rotate_vec3(box_pose->rotation, local_axis, world_axis);
        for (int s = -1; s <= 1; s += 2) {
            double y = (double)s * world_axis[1];
            if (isfinite(y) && y < best_y) {
                best_y = y;
                best_axis = axis;
                best_sign = (double)s;
            }
        }
    }
    if (axis_out)
        *axis_out = best_axis;
    if (sign_out)
        *sign_out = best_sign;
}

/// @brief Box-vs-heightfield narrow phase via a 3x3 probe of the lowest face.
///
/// Picks the box face whose world normal points most downward (so tumbled or
/// upside-down boxes still probe their true lowest geometry), then samples the
/// heightfield at that face's corners, edge midpoints, and center; the deepest
/// penetration wins. The edge samples catch narrow ridges that can pass between
/// the old corner+center probe pattern.
/// @param heightfield Heightfield Collider3D handle to sample.
/// @param field_pose World pose of the heightfield.
/// @param box Box proxy body to test.
/// @param normal Three-component output receiving the deepest field normal.
/// @param depth Output receiving normal-projected penetration depth.
/// @return One when at least one lowest-face sample penetrates, otherwise zero.
static int test_heightfield_box(void *heightfield,
                                const rt_collider_pose *field_pose,
                                const rt_body3d *box,
                                double *normal,
                                double *depth) {
    rt_collider_pose box_pose;
    double samples[9][3];
    double best_depth = 0.0;
    double best_normal[3] = {0.0, 1.0, 0.0};
    double best_normal_cos = 1.0;
    double local_samples[9][3];
    int sample_count = 0;
    int face_axis = 1;
    double face_sign = -1.0;
    if (!box)
        return 0;
    body_pose_from_proxy(box, &box_pose);
    heightfield_box_lowest_face(&box_pose, &face_axis, &face_sign);
    {
        int u = (face_axis + 1) % 3;
        int v = (face_axis + 2) % 3;
        for (int iu = -1; iu <= 1; ++iu) {
            for (int iv = -1; iv <= 1; ++iv) {
                double *sample = local_samples[sample_count++];
                sample[face_axis] = face_sign * box->half_extents[face_axis];
                sample[u] = (double)iu * box->half_extents[u];
                sample[v] = (double)iv * box->half_extents[v];
            }
        }
    }
    for (int i = 0; i < sample_count; i++)
        transform_point_from_pose(&box_pose, local_samples[i], samples[i]);
    for (int i = 0; i < sample_count; ++i) {
        double local_point[3];
        double surface_height = 0.0;
        double local_normal[3] = {0.0, 1.0, 0.0};
        transform_point_to_local(field_pose, samples[i], local_point);
        if (!rt_collider3d_sample_heightfield_raw(
                heightfield, local_point[0], local_point[2], &surface_height, local_normal))
            continue;
        {
            double cur_depth = surface_height - local_point[1];
            if (cur_depth > best_depth) {
                best_depth = cur_depth;
                best_normal_cos = clampd(local_normal[1], 0.0, 1.0);
                transform_normal_from_local(field_pose, local_normal, best_normal);
            }
        }
    }
    if (best_depth <= 0.0)
        return 0;
    {
        double len = vec3_len(best_normal);
        if (len > 1e-12) {
            best_normal[0] /= len;
            best_normal[1] /= len;
            best_normal[2] /= len;
        } else {
            best_normal[0] = 0.0;
            best_normal[1] = 1.0;
            best_normal[2] = 0.0;
        }
    }
    vec3_copy(normal, best_normal);
    /* The probe measures penetration vertically (local +Y); the solver pushes along
     * the surface normal, so project onto it (cos of the slope) or steep slopes get
     * a 1/cos-inflated ejection. */
    *depth = best_depth * best_normal_cos * pose_abs_scale_or_unit(field_pose->scale[1]);
    if (!isfinite(*depth) || *depth <= 0.0)
        return 0;
    return 1;
}

/// @brief Record the contributing leaf collider + pose for both bodies of a hit
///   (honoring NULL out-params) and report the pair as colliding. Centralizes the
///   identical bookkeeping every successful direct (non-swapped) dispatch case shares.
/// @param leaf_a_out Optional output receiving the first contributing leaf collider.
/// @param leaf_a_pose_out Optional output receiving the first leaf's world pose.
/// @param a_collider First contributing Collider3D.
/// @param a_pose World pose of @p a_collider.
/// @param leaf_b_out Optional output receiving the second contributing leaf collider.
/// @param leaf_b_pose_out Optional output receiving the second leaf's world pose.
/// @param b_collider Second contributing Collider3D.
/// @param b_pose World pose of @p b_collider.
/// @return One, indicating the already-tested pair collides.
static int commit_pair_leaves(void **leaf_a_out,
                              rt_collider_pose *leaf_a_pose_out,
                              void *a_collider,
                              const rt_collider_pose *a_pose,
                              void **leaf_b_out,
                              rt_collider_pose *leaf_b_pose_out,
                              void *b_collider,
                              const rt_collider_pose *b_pose) {
    if (leaf_a_out)
        *leaf_a_out = a_collider;
    if (leaf_a_pose_out)
        *leaf_a_pose_out = *a_pose;
    if (leaf_b_out)
        *leaf_b_out = b_collider;
    if (leaf_b_pose_out)
        *leaf_b_pose_out = *b_pose;
    return 1;
}

/// @brief Dispatch one collider pair, recursively descending compound children.
/// @param a_body First owning Body3D.
/// @param a_collider First Collider3D at the current recursion level.
/// @param a_pose World pose of @p a_collider.
/// @param b_body Second owning Body3D.
/// @param b_collider Second Collider3D at the current recursion level.
/// @param b_pose World pose of @p b_collider.
/// @param recursion_depth Current bounded compound recursion depth.
/// @param normal Output receiving the A-to-B contact normal.
/// @param depth Output receiving penetration depth.
/// @param leaf_a_out Optional output receiving the first contributing leaf.
/// @param leaf_a_pose_out Optional output receiving the first leaf pose.
/// @param leaf_b_out Optional output receiving the second contributing leaf.
/// @param leaf_b_pose_out Optional output receiving the second leaf pose.
/// @param manifold_acc Optional contact record accumulating compound leaf points.
/// @return One when a supported leaf pair overlaps, otherwise zero.
static int test_collider_pair(const rt_body3d *a_body,
                              void *a_collider,
                              const rt_collider_pose *a_pose,
                              const rt_body3d *b_body,
                              void *b_collider,
                              const rt_collider_pose *b_pose,
                              int32_t recursion_depth,
                              double *normal,
                              double *depth,
                              void **leaf_a_out,
                              rt_collider_pose *leaf_a_pose_out,
                              void **leaf_b_out,
                              rt_collider_pose *leaf_b_pose_out,
                              rt_contact3d *manifold_acc);

/// @brief Collide a compound collider (the @p compound_is_a side) against the other collider by
///        testing each child shape and keeping the deepest penetration. Mirrors the two formerly
///        duplicated compound branches of test_collider_pair; recurses back into it per child.
///        When @p manifold_acc is provided, every touching leaf contributes a manifold point
///        during THIS pass, so the solver no longer re-runs the whole compound narrow phase a
///        second time just to expand the manifold.
/// @param a_body First owning Body3D.
/// @param a_collider First collider at this recursion level.
/// @param a_pose World pose of @p a_collider.
/// @param b_body Second owning Body3D.
/// @param b_collider Second collider at this recursion level.
/// @param b_pose World pose of @p b_collider.
/// @param compound_is_a Nonzero when @p a_collider is the compound to descend.
/// @param recursion_depth Current bounded recursion depth.
/// @param normal Output receiving the deepest A-to-B contact normal.
/// @param depth Output receiving the deepest penetration.
/// @param leaf_a_out Optional output receiving the first deepest leaf.
/// @param leaf_a_pose_out Optional output receiving the first deepest leaf pose.
/// @param leaf_b_out Optional output receiving the second deepest leaf.
/// @param leaf_b_pose_out Optional output receiving the second deepest leaf pose.
/// @param manifold_acc Optional contact record accumulating all touching leaf points.
/// @return One when at least one compound child overlaps the other collider, otherwise zero.
static int test_compound_collider(const rt_body3d *a_body,
                                  void *a_collider,
                                  const rt_collider_pose *a_pose,
                                  const rt_body3d *b_body,
                                  void *b_collider,
                                  const rt_collider_pose *b_pose,
                                  int compound_is_a,
                                  int32_t recursion_depth,
                                  double *normal,
                                  double *depth,
                                  void **leaf_a_out,
                                  rt_collider_pose *leaf_a_pose_out,
                                  void **leaf_b_out,
                                  rt_collider_pose *leaf_b_pose_out,
                                  rt_contact3d *manifold_acc) {
    void *compound = compound_is_a ? a_collider : b_collider;
    const rt_collider_pose *compound_pose = compound_is_a ? a_pose : b_pose;
    double best_depth = 0.0;
    double best_normal[3] = {0.0, 1.0, 0.0};
    int hit = 0;
    int64_t child_count = rt_collider3d_get_child_count_raw(compound);
    for (int64_t i = 0; i < child_count; ++i) {
        void *child = rt_collider3d_get_child_raw(compound, i);
        double child_pos[3], child_rot[4], child_scale[3];
        rt_collider_pose child_pose;
        rt_collider3d_get_child_transform_raw(compound, i, child_pos, child_rot, child_scale);
        collider_pose_compose(compound_pose, child_pos, child_rot, child_scale, &child_pose);
        {
            double cur_normal[3];
            double cur_depth = 0.0;
            void *cur_leaf_a = NULL;
            void *cur_leaf_b = NULL;
            rt_collider_pose cur_leaf_a_pose;
            rt_collider_pose cur_leaf_b_pose;
            int sub = compound_is_a ? test_collider_pair(a_body,
                                                         child,
                                                         &child_pose,
                                                         b_body,
                                                         b_collider,
                                                         b_pose,
                                                         recursion_depth + 1,
                                                         cur_normal,
                                                         &cur_depth,
                                                         &cur_leaf_a,
                                                         &cur_leaf_a_pose,
                                                         &cur_leaf_b,
                                                         &cur_leaf_b_pose,
                                                         manifold_acc)
                                    : test_collider_pair(a_body,
                                                         a_collider,
                                                         a_pose,
                                                         b_body,
                                                         child,
                                                         &child_pose,
                                                         recursion_depth + 1,
                                                         cur_normal,
                                                         &cur_depth,
                                                         &cur_leaf_a,
                                                         &cur_leaf_a_pose,
                                                         &cur_leaf_b,
                                                         &cur_leaf_b_pose,
                                                         manifold_acc);
            if (sub && manifold_acc && cur_leaf_a && cur_leaf_b &&
                manifold_acc->contact_count < PH3D_MAX_MANIFOLD_POINTS) {
                double leaf_point[3];
                compute_contact_point_from_leafs(cur_leaf_a,
                                                 &cur_leaf_a_pose,
                                                 cur_leaf_b,
                                                 &cur_leaf_b_pose,
                                                 cur_normal,
                                                 leaf_point);
                contact3d_try_add_manifold_point(
                    manifold_acc, leaf_point, cur_normal, -cur_depth);
            }
            if (sub && cur_depth > best_depth) {
                best_depth = cur_depth;
                vec3_copy(best_normal, cur_normal);
                if (leaf_a_out)
                    *leaf_a_out = cur_leaf_a;
                if (leaf_a_pose_out)
                    *leaf_a_pose_out = cur_leaf_a_pose;
                if (leaf_b_out)
                    *leaf_b_out = cur_leaf_b;
                if (leaf_b_pose_out)
                    *leaf_b_pose_out = cur_leaf_b_pose;
                hit = 1;
            }
        }
    }
    if (hit) {
        vec3_copy(normal, best_normal);
        *depth = best_depth;
    }
    return hit;
}

/// @brief Broad-phase AABB reject for a collider pair: compute both world AABBs, return 0 when
///        they are disjoint, else fill the AABB extents and centers and return 1.
/// @param a_collider First Collider3D.
/// @param a_pose World pose of @p a_collider.
/// @param b_collider Second Collider3D.
/// @param b_pose World pose of @p b_collider.
/// @param amn Output receiving the first world AABB minimum.
/// @param amx Output receiving the first world AABB maximum.
/// @param bmn Output receiving the second world AABB minimum.
/// @param bmx Output receiving the second world AABB maximum.
/// @param a_center Output receiving the first AABB center.
/// @param b_center Output receiving the second AABB center.
/// @return One when the world AABBs overlap, otherwise zero.
static int collider_pair_broadphase(void *a_collider,
                                    const rt_collider_pose *a_pose,
                                    void *b_collider,
                                    const rt_collider_pose *b_pose,
                                    double amn[3],
                                    double amx[3],
                                    double bmn[3],
                                    double bmx[3],
                                    double a_center[3],
                                    double b_center[3]) {
    rt_collider3d_compute_world_aabb_raw(
        a_collider, a_pose->position, a_pose->rotation, a_pose->scale, amn, amx);
    rt_collider3d_compute_world_aabb_raw(
        b_collider, b_pose->position, b_pose->rotation, b_pose->scale, bmn, bmx);
    if (amn[0] > bmx[0] || amx[0] < bmn[0] || amn[1] > bmx[1] || amx[1] < bmn[1] ||
        amn[2] > bmx[2] || amx[2] < bmn[2])
        return 0;
    a_center[0] = (amn[0] + amx[0]) * 0.5;
    a_center[1] = (amn[1] + amx[1]) * 0.5;
    a_center[2] = (amn[2] + amx[2]) * 0.5;
    b_center[0] = (bmn[0] + bmx[0]) * 0.5;
    b_center[1] = (bmn[1] + bmx[1]) * 0.5;
    b_center[2] = (bmn[2] + bmx[2]) * 0.5;
    return 1;
}

/// @brief Narrow-phase collision test between two bodies' colliders: AABB broad-phase reject,
///   then per-shape GJK/EPA, returning the contact @p normal / @p depth and the deepest
///   contributing leaf colliders and their poses.
/// @details Recursively descends compound colliders, routes simple, convex,
///   mesh, and heightfield combinations to exact narrow phases, reverses
///   normals when symmetric dispatch swaps the pair, and fails closed for
///   unsupported shape combinations.
/// @param a_body First owning Body3D.
/// @param a_collider First Collider3D at the current recursion level.
/// @param a_pose World pose of @p a_collider.
/// @param b_body Second owning Body3D.
/// @param b_collider Second Collider3D at the current recursion level.
/// @param b_pose World pose of @p b_collider.
/// @param recursion_depth Current bounded compound recursion depth.
/// @param normal Output receiving the A-to-B contact normal.
/// @param depth Output receiving penetration depth.
/// @param leaf_a_out Optional output receiving the first contributing leaf.
/// @param leaf_a_pose_out Optional output receiving the first leaf pose.
/// @param leaf_b_out Optional output receiving the second contributing leaf.
/// @param leaf_b_pose_out Optional output receiving the second leaf pose.
/// @param manifold_acc Optional contact record accumulating compound leaf points.
/// @return 1 on contact, 0 otherwise.
static int test_collider_pair(const rt_body3d *a_body,
                              void *a_collider,
                              const rt_collider_pose *a_pose,
                              const rt_body3d *b_body,
                              void *b_collider,
                              const rt_collider_pose *b_pose,
                              int32_t recursion_depth,
                              double *normal,
                              double *depth,
                              void **leaf_a_out,
                              rt_collider_pose *leaf_a_pose_out,
                              void **leaf_b_out,
                              rt_collider_pose *leaf_b_pose_out,
                              rt_contact3d *manifold_acc) {
    double amn[3], amx[3], bmn[3], bmx[3];
    double a_center[3], b_center[3];
    int64_t a_type;
    int64_t b_type;
    if (!a_collider || !b_collider || recursion_depth > RT_COLLIDER3D_MAX_COMPOUND_DEPTH * 2)
        return 0;

    if (!collider_pair_broadphase(
            a_collider, a_pose, b_collider, b_pose, amn, amx, bmn, bmx, a_center, b_center))
        return 0;

    a_type = rt_collider3d_get_type(a_collider);
    b_type = rt_collider3d_get_type(b_collider);

    if (a_type == RT_COLLIDER3D_TYPE_COMPOUND)
        return test_compound_collider(a_body,
                                      a_collider,
                                      a_pose,
                                      b_body,
                                      b_collider,
                                      b_pose,
                                      1,
                                      recursion_depth,
                                      normal,
                                      depth,
                                      leaf_a_out,
                                      leaf_a_pose_out,
                                      leaf_b_out,
                                      leaf_b_pose_out,
                                      manifold_acc);

    if (b_type == RT_COLLIDER3D_TYPE_COMPOUND)
        return test_compound_collider(a_body,
                                      a_collider,
                                      a_pose,
                                      b_body,
                                      b_collider,
                                      b_pose,
                                      0,
                                      recursion_depth,
                                      normal,
                                      depth,
                                      leaf_a_out,
                                      leaf_a_pose_out,
                                      leaf_b_out,
                                      leaf_b_pose_out,
                                      manifold_acc);

    if (collider_type_is_simple(a_type) && collider_type_is_simple(b_type)) {
        if (!test_simple_collider_pose_collision(
                a_collider, a_pose, b_collider, b_pose, normal, depth))
            return 0;
        return commit_pair_leaves(leaf_a_out,
                                  leaf_a_pose_out,
                                  a_collider,
                                  a_pose,
                                  leaf_b_out,
                                  leaf_b_pose_out,
                                  b_collider,
                                  b_pose);
    }

    if (a_type == RT_COLLIDER3D_TYPE_CONVEX_HULL && collider_type_is_simple(b_type)) {
        if (!test_convex_hull_gjk_epa(
                a_collider, a_pose, b_collider, b_pose, a_center, b_center, normal, depth))
            return 0;
        return commit_pair_leaves(leaf_a_out,
                                  leaf_a_pose_out,
                                  a_collider,
                                  a_pose,
                                  leaf_b_out,
                                  leaf_b_pose_out,
                                  b_collider,
                                  b_pose);
    }

    if (a_type == RT_COLLIDER3D_TYPE_MESH && collider_type_is_simple(b_type)) {
        rt_body3d proxy_b;
        rt_mesh3d *mesh = (rt_mesh3d *)rt_collider3d_get_mesh_raw(a_collider);
        if (!build_simple_proxy(b_pose, b_collider, &proxy_b) || !mesh)
            return 0;
        if ((proxy_b.shape == PH3D_SHAPE_SPHERE &&
             !test_meshlike_sphere(mesh, a_pose, &proxy_b, normal, depth)) ||
            (proxy_b.shape == PH3D_SHAPE_CAPSULE &&
             !test_meshlike_capsule(mesh, a_pose, &proxy_b, normal, depth)) ||
            (proxy_b.shape == PH3D_SHAPE_AABB &&
             !test_meshlike_box(mesh, a_pose, &proxy_b, normal, depth)) ||
            ((proxy_b.shape != PH3D_SHAPE_SPHERE && proxy_b.shape != PH3D_SHAPE_CAPSULE &&
              proxy_b.shape != PH3D_SHAPE_AABB) &&
             !test_bounds_overlap(amn, amx, a_center, bmn, bmx, b_center, normal, depth))) {
            return 0;
        }
        return commit_pair_leaves(leaf_a_out,
                                  leaf_a_pose_out,
                                  a_collider,
                                  a_pose,
                                  leaf_b_out,
                                  leaf_b_pose_out,
                                  b_collider,
                                  b_pose);
    }

    if (a_type == RT_COLLIDER3D_TYPE_MESH && b_type == RT_COLLIDER3D_TYPE_CONVEX_HULL) {
        rt_mesh3d *mesh = (rt_mesh3d *)rt_collider3d_get_mesh_raw(a_collider);
        if (!mesh || !test_meshlike_convex_hull(mesh, a_pose, b_collider, b_pose, normal, depth))
            return 0;
        return commit_pair_leaves(leaf_a_out,
                                  leaf_a_pose_out,
                                  a_collider,
                                  a_pose,
                                  leaf_b_out,
                                  leaf_b_pose_out,
                                  b_collider,
                                  b_pose);
    }

    if (a_type == RT_COLLIDER3D_TYPE_CONVEX_HULL && b_type == RT_COLLIDER3D_TYPE_MESH) {
        int hit = test_collider_pair(b_body,
                                     b_collider,
                                     b_pose,
                                     a_body,
                                     a_collider,
                                     a_pose,
                                     recursion_depth,
                                     normal,
                                     depth,
                                     leaf_b_out,
                                     leaf_b_pose_out,
                                     leaf_a_out,
                                     leaf_a_pose_out,
                                     NULL);
        if (hit) {
            normal[0] = -normal[0];
            normal[1] = -normal[1];
            normal[2] = -normal[2];
        }
        return hit;
    }

    if (collider_type_is_simple(a_type) &&
        (b_type == RT_COLLIDER3D_TYPE_CONVEX_HULL || b_type == RT_COLLIDER3D_TYPE_MESH)) {
        int hit;
        hit = test_collider_pair(b_body,
                                 b_collider,
                                 b_pose,
                                 a_body,
                                 a_collider,
                                 a_pose,
                                 recursion_depth,
                                 normal,
                                 depth,
                                 leaf_b_out,
                                 leaf_b_pose_out,
                                 leaf_a_out,
                                 leaf_a_pose_out,
                                 NULL);
        if (hit) {
            normal[0] = -normal[0];
            normal[1] = -normal[1];
            normal[2] = -normal[2];
        }
        return hit;
    }

    if (a_type == RT_COLLIDER3D_TYPE_CONVEX_HULL && b_type == RT_COLLIDER3D_TYPE_CONVEX_HULL) {
        if (!test_convex_hull_gjk_epa(
                a_collider, a_pose, b_collider, b_pose, a_center, b_center, normal, depth))
            return 0;
        return commit_pair_leaves(leaf_a_out,
                                  leaf_a_pose_out,
                                  a_collider,
                                  a_pose,
                                  leaf_b_out,
                                  leaf_b_pose_out,
                                  b_collider,
                                  b_pose);
    }

    if (a_type == RT_COLLIDER3D_TYPE_HEIGHTFIELD && collider_type_is_simple(b_type)) {
        rt_body3d proxy_b;
        if (!build_simple_proxy(b_pose, b_collider, &proxy_b))
            return 0;
        if ((proxy_b.shape == PH3D_SHAPE_SPHERE &&
             !test_heightfield_sphere(a_collider, a_pose, &proxy_b, normal, depth)) ||
            (proxy_b.shape == PH3D_SHAPE_CAPSULE &&
             !test_heightfield_capsule(a_collider, a_pose, &proxy_b, normal, depth)) ||
            ((proxy_b.shape != PH3D_SHAPE_SPHERE && proxy_b.shape != PH3D_SHAPE_CAPSULE) &&
             !test_heightfield_box(a_collider, a_pose, &proxy_b, normal, depth))) {
            return 0;
        }
        return commit_pair_leaves(leaf_a_out,
                                  leaf_a_pose_out,
                                  a_collider,
                                  a_pose,
                                  leaf_b_out,
                                  leaf_b_pose_out,
                                  b_collider,
                                  b_pose);
    }

    if (collider_type_is_simple(a_type) && b_type == RT_COLLIDER3D_TYPE_HEIGHTFIELD) {
        int hit;
        hit = test_collider_pair(b_body,
                                 b_collider,
                                 b_pose,
                                 a_body,
                                 a_collider,
                                 a_pose,
                                 recursion_depth,
                                 normal,
                                 depth,
                                 leaf_b_out,
                                 leaf_b_pose_out,
                                 leaf_a_out,
                                 leaf_a_pose_out,
                                 NULL);
        if (hit) {
            normal[0] = -normal[0];
            normal[1] = -normal[1];
            normal[2] = -normal[2];
        }
        return hit;
    }

    /* Unsupported narrow-phase pair (mesh/mesh, mesh/heightfield, convex/heightfield, or
     * heightfield/heightfield). The AABB check above is only a reject test, never contact proof. */
    return 0;
}

/// @brief Collect distinct leaf contacts for a compound pair into an existing body-pair manifold.
/// @param contact Contact record whose bounded manifold is expanded.
/// @param a_body First owning Body3D.
/// @param a_collider First collider at the current recursion level.
/// @param a_pose World pose of @p a_collider.
/// @param b_body Second owning Body3D.
/// @param b_collider Second collider at the current recursion level.
/// @param b_pose World pose of @p b_collider.
/// @param recursion_depth Current bounded compound recursion depth.
static void contact3d_collect_compound_leaf_points(rt_contact3d *contact,
                                                   const rt_body3d *a_body,
                                                   void *a_collider,
                                                   const rt_collider_pose *a_pose,
                                                   const rt_body3d *b_body,
                                                   void *b_collider,
                                                   const rt_collider_pose *b_pose,
                                                   int32_t recursion_depth) {
    int64_t a_type;
    int64_t b_type;

    if (!contact || !a_collider || !a_pose || !b_collider || !b_pose ||
        contact->contact_count >= PH3D_MAX_MANIFOLD_POINTS ||
        recursion_depth > RT_COLLIDER3D_MAX_COMPOUND_DEPTH * 2)
        return;
    a_type = rt_collider3d_get_type(a_collider);
    b_type = rt_collider3d_get_type(b_collider);

    if (a_type == RT_COLLIDER3D_TYPE_COMPOUND) {
        int64_t child_count = rt_collider3d_get_child_count_raw(a_collider);
        for (int64_t i = 0; i < child_count && contact->contact_count < PH3D_MAX_MANIFOLD_POINTS;
             ++i) {
            double child_pos[3];
            double child_rot[4];
            double child_scale[3];
            rt_collider_pose child_pose;
            void *child = rt_collider3d_get_child_raw(a_collider, i);

            rt_collider3d_get_child_transform_raw(a_collider, i, child_pos, child_rot, child_scale);
            collider_pose_compose(a_pose, child_pos, child_rot, child_scale, &child_pose);
            contact3d_collect_compound_leaf_points(contact,
                                                   a_body,
                                                   child,
                                                   &child_pose,
                                                   b_body,
                                                   b_collider,
                                                   b_pose,
                                                   recursion_depth + 1);
        }
        return;
    }
    if (b_type == RT_COLLIDER3D_TYPE_COMPOUND) {
        int64_t child_count = rt_collider3d_get_child_count_raw(b_collider);
        for (int64_t i = 0; i < child_count && contact->contact_count < PH3D_MAX_MANIFOLD_POINTS;
             ++i) {
            double child_pos[3];
            double child_rot[4];
            double child_scale[3];
            rt_collider_pose child_pose;
            void *child = rt_collider3d_get_child_raw(b_collider, i);

            rt_collider3d_get_child_transform_raw(b_collider, i, child_pos, child_rot, child_scale);
            collider_pose_compose(b_pose, child_pos, child_rot, child_scale, &child_pose);
            contact3d_collect_compound_leaf_points(contact,
                                                   a_body,
                                                   a_collider,
                                                   a_pose,
                                                   b_body,
                                                   child,
                                                   &child_pose,
                                                   recursion_depth + 1);
        }
        return;
    }

    {
        double normal[3];
        double depth = 0.0;
        double point[3];
        void *leaf_a = NULL;
        void *leaf_b = NULL;
        rt_collider_pose leaf_a_pose;
        rt_collider_pose leaf_b_pose;

        if (!test_collider_pair(a_body,
                                a_collider,
                                a_pose,
                                b_body,
                                b_collider,
                                b_pose,
                                recursion_depth,
                                normal,
                                &depth,
                                &leaf_a,
                                &leaf_a_pose,
                                &leaf_b,
                                &leaf_b_pose,
                                NULL))
            return;
        compute_contact_point_from_leafs(leaf_a, &leaf_a_pose, leaf_b, &leaf_b_pose, normal, point);
        contact3d_try_add_manifold_point(contact, point, normal, -depth);
    }
}

/// @brief Add contacts from multiple touching compound leaves to a body-pair manifold.
/// @param contact Existing contact record to expand.
/// @param a First Body3D in the pair.
/// @param b Second Body3D in the pair.
void contact3d_expand_compound_manifold(rt_contact3d *contact,
                                        const rt_body3d *a,
                                        const rt_body3d *b) {
    rt_collider_pose a_pose;
    rt_collider_pose b_pose;
    int64_t a_type;
    int64_t b_type;

    if (!contact || !a || !b || !a->collider || !b->collider ||
        contact->contact_count >= PH3D_MAX_MANIFOLD_POINTS)
        return;
    a_type = rt_collider3d_get_type(a->collider);
    b_type = rt_collider3d_get_type(b->collider);
    if (a_type != RT_COLLIDER3D_TYPE_COMPOUND && b_type != RT_COLLIDER3D_TYPE_COMPOUND)
        return;
    collider_pose_from_body(a, &a_pose);
    collider_pose_from_body(b, &b_pose);
    contact3d_collect_compound_leaf_points(
        contact, a, a->collider, &a_pose, b, b->collider, &b_pose, 0);
}

/// @brief Top-level body-vs-body collision test, including contact point.
///
/// Builds collider poses, delegates to `test_collider_pair` for the
/// actual recursion, then reconstructs an estimated contact point via
/// `compute_contact_point_from_leafs`. Outputs the leaf colliders that
/// touched, so contact events can carry sub-collider identity.
/// @param a First Body3D, establishing the normal origin.
/// @param b Second Body3D, toward which the contact normal points.
/// @param normal Output receiving the A-to-B contact normal.
/// @param depth Output receiving penetration depth.
/// @param point Optional output receiving a representative world contact point.
/// @param leaf_a_out Optional output receiving the first contributing leaf collider.
/// @param leaf_b_out Optional output receiving the second contributing leaf collider.
/// @param leaf_a_pose_out Optional output receiving the first leaf's world pose.
/// @param leaf_b_pose_out Optional output receiving the second leaf's world pose.
/// @param manifold_acc Optional contact record accumulating compound leaf points.
/// @return One when the bodies collide, otherwise zero.
int test_collision_manifold(const rt_body3d *a,
                            const rt_body3d *b,
                            double *normal,
                            double *depth,
                            double *point,
                            void **leaf_a_out,
                            void **leaf_b_out,
                            rt_collider_pose *leaf_a_pose_out,
                            rt_collider_pose *leaf_b_pose_out,
                            rt_contact3d *manifold_acc) {
    rt_collider_pose a_pose;
    rt_collider_pose b_pose;
    rt_collider_pose leaf_a_pose;
    rt_collider_pose leaf_b_pose;
    void *leaf_a = NULL;
    void *leaf_b = NULL;
    if (!a || !b)
        return 0;
    if (!a->collider || !b->collider) {
        if (!body3d_has_collision_geometry(a) || !body3d_has_collision_geometry(b))
            return 0;
        if (!test_simple_collision(a, b, normal, depth))
            return 0;
        if (leaf_a_out)
            *leaf_a_out = NULL;
        if (leaf_b_out)
            *leaf_b_out = NULL;
        if (leaf_a_pose_out)
            collider_pose_identity(leaf_a_pose_out);
        if (leaf_b_pose_out)
            collider_pose_identity(leaf_b_pose_out);
        if (point)
            compute_contact_point_from_body_aabbs(a, b, normal, point);
        return 1;
    }
    collider_pose_from_body(a, &a_pose);
    collider_pose_from_body(b, &b_pose);
    if (!test_collider_pair(a,
                            a->collider,
                            &a_pose,
                            b,
                            b->collider,
                            &b_pose,
                            0,
                            normal,
                            depth,
                            &leaf_a,
                            &leaf_a_pose,
                            &leaf_b,
                            &leaf_b_pose,
                            manifold_acc))
        return 0;
    if (leaf_a_out)
        *leaf_a_out = leaf_a;
    if (leaf_b_out)
        *leaf_b_out = leaf_b;
    if (leaf_a_pose_out)
        *leaf_a_pose_out = leaf_a_pose;
    if (leaf_b_pose_out)
        *leaf_b_pose_out = leaf_b_pose;
    if (point)
        compute_contact_point_from_leafs(leaf_a, &leaf_a_pose, leaf_b, &leaf_b_pose, normal, point);
    return 1;
}

/// @brief Top-level body-vs-body collision test, including contact point.
///
/// Thin wrapper over test_collision_manifold with no manifold accumulator —
/// queries and probes only need the single deepest result.
/// @param a First Body3D, establishing the normal origin.
/// @param b Second Body3D, toward which the contact normal points.
/// @param normal Output receiving the A-to-B contact normal.
/// @param depth Output receiving penetration depth.
/// @param point Optional output receiving a representative world contact point.
/// @param leaf_a_out Optional output receiving the first contributing leaf collider.
/// @param leaf_b_out Optional output receiving the second contributing leaf collider.
/// @param leaf_a_pose_out Optional output receiving the first leaf's world pose.
/// @param leaf_b_pose_out Optional output receiving the second leaf's world pose.
/// @return One when the bodies collide, otherwise zero.
int test_collision(const rt_body3d *a,
                   const rt_body3d *b,
                   double *normal,
                   double *depth,
                   double *point,
                   void **leaf_a_out,
                   void **leaf_b_out,
                   rt_collider_pose *leaf_a_pose_out,
                   rt_collider_pose *leaf_b_pose_out) {
    return test_collision_manifold(
        a, b, normal, depth, point, leaf_a_out, leaf_b_out, leaf_a_pose_out, leaf_b_pose_out, NULL);
}
