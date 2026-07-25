//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_triangulation.inc
 * @brief Provides recoverable, scale-aware triangulation for imported FBX polygons.
 *
 * Polygon vertices are projected onto the dominant Newell-normal plane and ear-clipped
 * with a bounded quadratic guard. Degenerate or excessively large polygons fall back
 * to a triangle fan. Every candidate triangle is validated without invoking the
 * script-facing Mesh3D traps, allowing malformed asset geometry to be skipped or failed
 * through ordinary loader status paths.
 */

/// @brief Project a mesh vertex to 2D by dropping one axis (0=X, 1=Y, else Z),
///   yielding the (out_x, out_y) plane coordinates used for in-plane triangulation.
/// @param mesh Source runtime mesh.
/// @param vertex_index Valid vertex index in @p mesh.
/// @param drop_axis Axis omitted from the projection: zero for X, one for Y, otherwise Z.
/// @param[out] out_x Receives the first projected coordinate.
/// @param[out] out_y Receives the second projected coordinate.
static void fbx_project_mesh_vertex(
    const rt_mesh3d *mesh, int32_t vertex_index, int drop_axis, double *out_x, double *out_y) {
    const float *p = mesh->vertices[vertex_index].pos;
    if (drop_axis == 0) {
        *out_x = p[1];
        *out_y = p[2];
    } else if (drop_axis == 1) {
        *out_x = p[0];
        *out_y = p[2];
    } else {
        *out_x = p[0];
        *out_y = p[1];
    }
}

/// @brief Twice the signed area of triangle (a,b,c) — the 2D cross product of edges
///   ab and ac. Positive for counter-clockwise winding, negative for clockwise.
/// @param ax Vertex A X coordinate.
/// @param ay Vertex A Y coordinate.
/// @param bx Vertex B X coordinate.
/// @param by Vertex B Y coordinate.
/// @param cx Vertex C X coordinate.
/// @param cy Vertex C Y coordinate.
/// @return Twice the signed projected area.
static double fbx_tri_area2(double ax, double ay, double bx, double by, double cx, double cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/// @brief True if point (px,py) lies inside triangle (a,b,c), winding-agnostic.
/// @details Tests the point against each edge; inside iff the three signed areas
///   share one sign (an epsilon tolerance treats edge-coincident points as inside).
/// @param px Point X coordinate.
/// @param py Point Y coordinate.
/// @param ax Triangle vertex A X coordinate.
/// @param ay Triangle vertex A Y coordinate.
/// @param bx Triangle vertex B X coordinate.
/// @param by Triangle vertex B Y coordinate.
/// @param cx Triangle vertex C X coordinate.
/// @param cy Triangle vertex C Y coordinate.
/// @return Non-zero when the point is inside or on an edge of the triangle.
static int fbx_point_in_tri_2d(
    double px, double py, double ax, double ay, double bx, double by, double cx, double cy) {
    double ab = fbx_tri_area2(ax, ay, bx, by, px, py);
    double bc = fbx_tri_area2(bx, by, cx, cy, px, py);
    double ca = fbx_tri_area2(cx, cy, ax, ay, px, py);
    int has_neg = (ab < -1e-12) || (bc < -1e-12) || (ca < -1e-12);
    int has_pos = (ab > 1e-12) || (bc > 1e-12) || (ca > 1e-12);
    return !(has_neg && has_pos);
}

/// @brief Compute squared 3D double-area using authoritative mesh positions when present.
/// @param mesh Source runtime mesh.
/// @param i0 First valid vertex index.
/// @param i1 Second valid vertex index.
/// @param i2 Third valid vertex index.
/// @param[out] a Receives vertex A as doubles.
/// @param[out] b Receives vertex B as doubles.
/// @param[out] c Receives vertex C as doubles.
/// @return Squared magnitude of `(B - A) cross (C - A)`, equal to squared double-area.
static double fbx_mesh_tri_area_sq(const rt_mesh3d *mesh,
                                   int32_t i0,
                                   int32_t i1,
                                   int32_t i2,
                                   double a[3],
                                   double b[3],
                                   double c[3]) {
    double abx;
    double aby;
    double abz;
    double acx;
    double acy;
    double acz;
    double nx;
    double ny;
    double nz;
    if (mesh->positions64) {
        const double *p0 = &mesh->positions64[(size_t)i0 * 3u];
        const double *p1 = &mesh->positions64[(size_t)i1 * 3u];
        const double *p2 = &mesh->positions64[(size_t)i2 * 3u];
        a[0] = p0[0];
        a[1] = p0[1];
        a[2] = p0[2];
        b[0] = p1[0];
        b[1] = p1[1];
        b[2] = p1[2];
        c[0] = p2[0];
        c[1] = p2[1];
        c[2] = p2[2];
    } else {
        const float *p0 = mesh->vertices[i0].pos;
        const float *p1 = mesh->vertices[i1].pos;
        const float *p2 = mesh->vertices[i2].pos;
        a[0] = (double)p0[0];
        a[1] = (double)p0[1];
        a[2] = (double)p0[2];
        b[0] = (double)p1[0];
        b[1] = (double)p1[1];
        b[2] = (double)p1[2];
        c[0] = (double)p2[0];
        c[1] = (double)p2[1];
        c[2] = (double)p2[2];
    }
    abx = b[0] - a[0];
    aby = b[1] - a[1];
    abz = b[2] - a[2];
    acx = c[0] - a[0];
    acy = c[1] - a[1];
    acz = c[2] - a[2];
    nx = aby * acz - abz * acy;
    ny = abz * acx - abx * acz;
    nz = abx * acy - aby * acx;
    return nx * nx + ny * ny + nz * nz;
}

/// @brief Compute a scale-aware squared-area threshold matching Mesh3D degeneracy policy.
/// @param a Triangle vertex A.
/// @param b Triangle vertex B.
/// @param c Triangle vertex C.
/// @return Finite positive squared-area epsilon derived from the longest edge.
static double fbx_mesh_tri_area_epsilon_sq(const double a[3],
                                           const double b[3],
                                           const double c[3]) {
    double abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
    double acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
    double bcx = c[0] - b[0], bcy = c[1] - b[1], bcz = c[2] - b[2];
    double ab2 = abx * abx + aby * aby + abz * abz;
    double ac2 = acx * acx + acy * acy + acz * acz;
    double bc2 = bcx * bcx + bcy * bcy + bcz * bcz;
    double max_edge_sq = fmax(ab2, fmax(ac2, bc2));
    if (!isfinite(max_edge_sq) || max_edge_sq <= 0.0)
        return 1e-24;
    return fmax(1e-24, max_edge_sq * max_edge_sq * 1e-12);
}

/// @brief Emit a triangle from untrusted FBX geometry without invoking Mesh3D traps.
/// @details Duplicate, out-of-range, and zero-area imported triangles are dropped. The public
/// Mesh3D.AddTriangle API still traps for script errors; loader paths must fail recoverably.
/// @param[in,out] mesh_obj Runtime Mesh3D receiving valid indices.
/// @param i0 First candidate vertex index.
/// @param i1 Second candidate vertex index.
/// @param i2 Third candidate vertex index.
/// @return Non-zero when the triangle was emitted or recoverably dropped; zero only
///         when the mesh is invalid or its build state fails.
static int fbx_try_emit_mesh_triangle(void *mesh_obj, int32_t i0, int32_t i1, int32_t i2) {
    rt_mesh3d *mesh = (rt_mesh3d *)mesh_obj;
    double a[3];
    double b[3];
    double c[3];
    double area_sq;
    if (!mesh || !mesh->vertices)
        return 0;
    if (i0 < 0 || i1 < 0 || i2 < 0 || i0 == i1 || i1 == i2 || i0 == i2)
        return 1;
    if ((uint32_t)i0 >= mesh->vertex_count || (uint32_t)i1 >= mesh->vertex_count ||
        (uint32_t)i2 >= mesh->vertex_count)
        return 1;
    area_sq = fbx_mesh_tri_area_sq(mesh, i0, i1, i2, a, b, c);
    if (!isfinite(area_sq) || area_sq <= fbx_mesh_tri_area_epsilon_sq(a, b, c))
        return 1;
    rt_mesh3d_add_triangle(mesh_obj, i0, i1, i2);
    return mesh->build_failed ? 0 : 1;
}

/// @brief Largest N-gon the ear-clipper will process. Beyond this a triangle fan is
///   used: the O(n^2) ear-clip becomes too costly and `poly_count * poly_count` would
///   overflow the loop guard for untrusted/degenerate polygons.
#define FBX_MAX_EARCLIP_POLY_VERTS 1024

/// @brief Emit a polygon as a triangle fan anchored at vertex 0.
/// @details Safe fallback for degenerate or pathologically large N-gons. Index
///   validation is handled by fbx_try_emit_mesh_triangle.
/// @param[in,out] mesh_obj Runtime Mesh3D receiving valid triangles.
/// @param polygon Ordered emitted-vertex index array.
/// @param poly_count Number of vertices in @p polygon.
/// @return 0 on mesh build failure, 1 otherwise.
static int fbx_emit_polygon_fan(void *mesh_obj, const int32_t *polygon, int32_t poly_count) {
    const rt_mesh3d *mesh = (const rt_mesh3d *)mesh_obj;
    for (int32_t fi = 1; fi < poly_count - 1; ++fi)
        if (!fbx_try_emit_mesh_triangle(mesh_obj, polygon[0], polygon[fi], polygon[fi + 1]))
            return 0;
    return mesh->build_failed ? 0 : 1;
}

/// @brief Triangulate an FBX polygon of `poly_count` vertices and emit the triangles
///   into `mesh`. Returns 0 on allocation or mesh build failure, 1 otherwise.
/// @details Fast-paths triangles; for N-gons it derives a projection plane from the
///   Newell normal, then ear-clips in 2D (a poly_count² guard bounds the loop). On a
///   degenerate/zero-area polygon or stalled clip it falls back to a naive triangle fan.
/// @param[in,out] mesh_obj Runtime Mesh3D receiving generated triangles.
/// @param polygon Ordered emitted-vertex index array.
/// @param poly_count Number of vertices in @p polygon.
/// @return Non-zero on success or a recoverable invalid/undersized polygon; zero on
///         allocation or mesh build failure.
static int fbx_emit_polygon_triangles(void *mesh_obj, const int32_t *polygon, int32_t poly_count) {
    rt_mesh3d *mesh = (rt_mesh3d *)mesh_obj;
    int32_t stack_indices[64];
    int32_t *indices = stack_indices;
    double normal[3] = {0.0, 0.0, 0.0};
    double area = 0.0;
    int drop_axis = 2;
    int ccw;
    int32_t remaining;
    int64_t guard;
    if (!mesh || !polygon || poly_count < 3)
        return 1;
    if (poly_count == 3) {
        return fbx_try_emit_mesh_triangle(mesh_obj, polygon[0], polygon[1], polygon[2]);
    }
    if (poly_count > FBX_MAX_EARCLIP_POLY_VERTS) {
        return fbx_emit_polygon_fan(mesh_obj, polygon, poly_count);
    }
    if ((size_t)poly_count > sizeof(stack_indices) / sizeof(stack_indices[0])) {
        if ((size_t)poly_count > SIZE_MAX / sizeof(int32_t))
            return 0;
        indices = (int32_t *)malloc((size_t)poly_count * sizeof(int32_t));
        if (!indices)
            return 0;
    }
    for (int32_t i = 0; i < poly_count; ++i) {
        const float *a = mesh->vertices[polygon[i]].pos;
        const float *b = mesh->vertices[polygon[(i + 1) % poly_count]].pos;
        normal[0] += ((double)a[1] - (double)b[1]) * ((double)a[2] + (double)b[2]);
        normal[1] += ((double)a[2] - (double)b[2]) * ((double)a[0] + (double)b[0]);
        normal[2] += ((double)a[0] - (double)b[0]) * ((double)a[1] + (double)b[1]);
    }
    if (fabs(normal[0]) >= fabs(normal[1]) && fabs(normal[0]) >= fabs(normal[2]))
        drop_axis = 0;
    else if (fabs(normal[1]) >= fabs(normal[2]))
        drop_axis = 1;
    for (int32_t i = 0; i < poly_count; ++i) {
        double ax, ay, bx, by;
        fbx_project_mesh_vertex(mesh, polygon[i], drop_axis, &ax, &ay);
        fbx_project_mesh_vertex(mesh, polygon[(i + 1) % poly_count], drop_axis, &bx, &by);
        area += ax * by - bx * ay;
        indices[i] = polygon[i];
    }
    if (!isfinite(area) || fabs(area) < 1e-12) {
        int fan_ok = fbx_emit_polygon_fan(mesh_obj, polygon, poly_count);
        if (indices != stack_indices)
            free(indices);
        return fan_ok;
    }
    ccw = area > 0.0;
    remaining = poly_count;
    guard = (int64_t)poly_count * (int64_t)poly_count;
    while (remaining > 3 && guard-- > 0) {
        int clipped = 0;
        for (int32_t i = 0; i < remaining; ++i) {
            int32_t prev_i = (i + remaining - 1) % remaining;
            int32_t next_i = (i + 1) % remaining;
            int32_t ia = indices[prev_i];
            int32_t ib = indices[i];
            int32_t ic = indices[next_i];
            double ax, ay, bx, by, cx, cy;
            double turn;
            int contains = 0;
            fbx_project_mesh_vertex(mesh, ia, drop_axis, &ax, &ay);
            fbx_project_mesh_vertex(mesh, ib, drop_axis, &bx, &by);
            fbx_project_mesh_vertex(mesh, ic, drop_axis, &cx, &cy);
            turn = fbx_tri_area2(ax, ay, bx, by, cx, cy);
            if ((ccw && turn <= 1e-12) || (!ccw && turn >= -1e-12))
                continue;
            for (int32_t j = 0; j < remaining; ++j) {
                double px, py;
                if (j == prev_i || j == i || j == next_i)
                    continue;
                fbx_project_mesh_vertex(mesh, indices[j], drop_axis, &px, &py);
                if (fbx_point_in_tri_2d(px, py, ax, ay, bx, by, cx, cy)) {
                    contains = 1;
                    break;
                }
            }
            if (contains)
                continue;
            if (!fbx_try_emit_mesh_triangle(mesh_obj, ia, ib, ic)) {
                if (indices != stack_indices)
                    free(indices);
                return 0;
            }
            for (int32_t j = i; j < remaining - 1; ++j)
                indices[j] = indices[j + 1];
            remaining--;
            clipped = 1;
            break;
        }
        if (!clipped)
            break;
    }
    if (remaining == 3) {
        if (!fbx_try_emit_mesh_triangle(mesh_obj, indices[0], indices[1], indices[2])) {
            if (indices != stack_indices)
                free(indices);
            return 0;
        }
    } else {
        for (int32_t fi = 1; fi < poly_count - 1; ++fi)
            if (!fbx_try_emit_mesh_triangle(mesh_obj, polygon[0], polygon[fi], polygon[fi + 1])) {
                if (indices != stack_indices)
                    free(indices);
                return 0;
            }
    }
    if (indices != stack_indices)
        free(indices);
    return mesh->build_failed ? 0 : 1;
}
