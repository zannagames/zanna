//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/render/rt_mesh3d_procedural.inc  (#included from rt_mesh3d.c; not a
// standalone translation unit) Purpose: Mesh3D procedural generators: box, sphere, plane, cylinder
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements bounded procedural Mesh3D box, sphere, plane, and cylinder generators.
/// @details This include fragment is compiled as part of `rt_mesh3d.c`. Each generator validates
///          dimensions, preflights storage, batches geometry mutations, and returns either a
///          complete CCW-wound mesh or NULL after releasing a failed partial build.

/* Procedural generators — NewBox, NewSphere, NewPlane, NewCylinder */
/// @brief Generate a box mesh centered at the origin with full size (sx, sy, sz).
/// @details Emits four independent vertices per face so each face has a constant outward normal
///          and full rectangular UV chart. The result contains 24 vertices and 12 triangles.
/// @param sx Positive finite full extent along X.
/// @param sy Positive finite full extent along Y.
/// @param sz Positive finite full extent along Z.
/// @return New box Mesh3D, or NULL for invalid dimensions or allocation/build failure.
void *rt_mesh3d_new_box(double sx, double sy, double sz) {
    if (!mesh_validate_positive_finite(sx, "Mesh3D.NewBox: sx") ||
        !mesh_validate_positive_finite(sy, "Mesh3D.NewBox: sy") ||
        !mesh_validate_positive_finite(sz, "Mesh3D.NewBox: sz"))
        return NULL;
    void *m = rt_mesh3d_new();
    if (!m)
        return NULL;
    if (!mesh3d_reserve_storage((rt_mesh3d *)m, 24u, 36u, "Mesh3D.NewBox")) {
        mesh_mark_build_failed((rt_mesh3d *)m);
        return mesh_return_null_if_build_failed(m);
    }

    double hx = sx * 0.5, hy = sy * 0.5, hz = sz * 0.5;

    /* 6 faces, 4 verts each = 24 verts, 12 triangles (CCW winding) */
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)m);
    /* Front face (+Z) */
    rt_mesh3d_add_vertex(m, -hx, -hy, hz, 0, 0, 1, 0, 1);
    rt_mesh3d_add_vertex(m, hx, -hy, hz, 0, 0, 1, 1, 1);
    rt_mesh3d_add_vertex(m, hx, hy, hz, 0, 0, 1, 1, 0);
    rt_mesh3d_add_vertex(m, -hx, hy, hz, 0, 0, 1, 0, 0);
    rt_mesh3d_add_triangle(m, 0, 1, 2);
    rt_mesh3d_add_triangle(m, 0, 2, 3);

    /* Back face (-Z) */
    rt_mesh3d_add_vertex(m, hx, -hy, -hz, 0, 0, -1, 0, 1);
    rt_mesh3d_add_vertex(m, -hx, -hy, -hz, 0, 0, -1, 1, 1);
    rt_mesh3d_add_vertex(m, -hx, hy, -hz, 0, 0, -1, 1, 0);
    rt_mesh3d_add_vertex(m, hx, hy, -hz, 0, 0, -1, 0, 0);
    rt_mesh3d_add_triangle(m, 4, 5, 6);
    rt_mesh3d_add_triangle(m, 4, 6, 7);

    /* Right face (+X) */
    rt_mesh3d_add_vertex(m, hx, -hy, hz, 1, 0, 0, 0, 1);
    rt_mesh3d_add_vertex(m, hx, -hy, -hz, 1, 0, 0, 1, 1);
    rt_mesh3d_add_vertex(m, hx, hy, -hz, 1, 0, 0, 1, 0);
    rt_mesh3d_add_vertex(m, hx, hy, hz, 1, 0, 0, 0, 0);
    rt_mesh3d_add_triangle(m, 8, 9, 10);
    rt_mesh3d_add_triangle(m, 8, 10, 11);

    /* Left face (-X) */
    rt_mesh3d_add_vertex(m, -hx, -hy, -hz, -1, 0, 0, 0, 1);
    rt_mesh3d_add_vertex(m, -hx, -hy, hz, -1, 0, 0, 1, 1);
    rt_mesh3d_add_vertex(m, -hx, hy, hz, -1, 0, 0, 1, 0);
    rt_mesh3d_add_vertex(m, -hx, hy, -hz, -1, 0, 0, 0, 0);
    rt_mesh3d_add_triangle(m, 12, 13, 14);
    rt_mesh3d_add_triangle(m, 12, 14, 15);

    /* Top face (+Y) */
    rt_mesh3d_add_vertex(m, -hx, hy, hz, 0, 1, 0, 0, 1);
    rt_mesh3d_add_vertex(m, hx, hy, hz, 0, 1, 0, 1, 1);
    rt_mesh3d_add_vertex(m, hx, hy, -hz, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, -hx, hy, -hz, 0, 1, 0, 0, 0);
    rt_mesh3d_add_triangle(m, 16, 17, 18);
    rt_mesh3d_add_triangle(m, 16, 18, 19);

    /* Bottom face (-Y) */
    rt_mesh3d_add_vertex(m, -hx, -hy, -hz, 0, -1, 0, 0, 1);
    rt_mesh3d_add_vertex(m, hx, -hy, -hz, 0, -1, 0, 1, 1);
    rt_mesh3d_add_vertex(m, hx, -hy, hz, 0, -1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, -hx, -hy, hz, 0, -1, 0, 0, 0);
    rt_mesh3d_add_triangle(m, 20, 21, 22);
    rt_mesh3d_add_triangle(m, 20, 22, 23);
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)m);

    return mesh_return_null_if_build_failed(m);
}

/// @brief Generate a UV sphere mesh with the given radius and segment count.
/// @details Uses single pole vertices and seam-duplicated intermediate rings to preserve UV
///          continuity without zero-area cap triangles. Segment counts below four are promoted to
///          four and counts above the configured safety maximum trap.
/// @param radius Positive finite sphere radius.
/// @param segments Requested latitude-ring count; longitude uses twice this sanitized count.
/// @return New UV sphere Mesh3D, or NULL for invalid dimensions, excessive complexity, or
///         allocation/build failure.
void *rt_mesh3d_new_sphere(double radius, int64_t segments) {
    if (!mesh_validate_positive_finite(radius, "Mesh3D.NewSphere: radius"))
        return NULL;
    if (segments > MESH_MAX_SPHERE_SEGMENTS) {
        rt_trap("Mesh3D.NewSphere: segments exceeds safe limit");
        return NULL;
    }
    void *m = rt_mesh3d_new();
    if (!m)
        return NULL;
    if (segments < 4)
        segments = 4;

    int64_t rings = segments;
    int64_t slices = segments * 2;
    double r = radius;
    int64_t top_index = 0;
    int64_t first_ring = 1;
    int64_t ring_stride = slices + 1;
    int64_t bottom_index;
    uint32_t vertex_capacity = (uint32_t)(2 + (rings - 1) * ring_stride);
    uint32_t index_capacity = (uint32_t)(slices * 3 + (rings - 2) * slices * 6 + slices * 3);
    float *slice_trig = NULL;
    float *ring_trig = NULL;
    if (!mesh3d_check_planned_payload(
            vertex_capacity, index_capacity, "Mesh3D.NewSphere: mesh exceeds memory budget")) {
        mesh_mark_build_failed((rt_mesh3d *)m);
        return mesh_return_null_if_build_failed(m);
    }
    if (!mesh3d_reserve_storage(
            (rt_mesh3d *)m, vertex_capacity, index_capacity, "Mesh3D.NewSphere")) {
        mesh_mark_build_failed((rt_mesh3d *)m);
        return mesh_return_null_if_build_failed(m);
    }
    slice_trig = (float *)malloc((size_t)(slices + 1) * 2u * sizeof(float));
    ring_trig = (float *)malloc((size_t)(rings + 1) * 2u * sizeof(float));
    if (!slice_trig || !ring_trig) {
        free(slice_trig);
        free(ring_trig);
        mesh_mark_build_failed((rt_mesh3d *)m);
        rt_trap("Mesh3D.NewSphere: trigonometry table allocation failed");
        return mesh_return_null_if_build_failed(m);
    }
    for (int64_t slice = 0; slice <= slices; slice++) {
        float theta = 2.0f * (float)M_PI * (float)slice / (float)slices;
        slice_trig[(size_t)slice * 2u + 0u] = cosf(theta);
        slice_trig[(size_t)slice * 2u + 1u] = sinf(theta);
    }
    for (int64_t ring = 0; ring <= rings; ring++) {
        float phi = (float)M_PI * (float)ring / (float)rings;
        ring_trig[(size_t)ring * 2u + 0u] = cosf(phi);
        ring_trig[(size_t)ring * 2u + 1u] = sinf(phi);
    }

    /* Generate a single pole vertex at each end, and seam-duplicated body rings.
     * This keeps UV seams intact without emitting zero-area cap triangles. */
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)m);
    rt_mesh3d_add_vertex(m, 0.0, r, 0.0, 0.0, 1.0, 0.0, 0.5, 0.0);
    for (int64_t ring = 1; ring < rings; ring++) {
        float cp = ring_trig[(size_t)ring * 2u + 0u];
        float sp = ring_trig[(size_t)ring * 2u + 1u];

        for (int64_t slice = 0; slice <= slices; slice++) {
            float ct = slice_trig[(size_t)slice * 2u + 0u];
            float st = slice_trig[(size_t)slice * 2u + 1u];

            float nx = sp * ct, ny = cp, nz = sp * st;
            float u = (float)slice / (float)slices;
            float v = (float)ring / (float)rings;
            rt_mesh3d_add_vertex(m, nx * r, ny * r, nz * r, nx, ny, nz, u, v);
        }
    }
    bottom_index = (int64_t)((rt_mesh3d *)m)->vertex_count;
    rt_mesh3d_add_vertex(m, 0.0, -r, 0.0, 0.0, -1.0, 0.0, 0.5, 1.0);

    /* Generate indices (CCW) */
    for (int64_t slice = 0; slice < slices; slice++) {
        int64_t b = first_ring + slice;
        rt_mesh3d_add_triangle(m, top_index, b + 1, b);
    }
    for (int64_t ring = 1; ring < rings - 1; ring++) {
        int64_t a_base = first_ring + (ring - 1) * ring_stride;
        int64_t b_base = a_base + ring_stride;
        for (int64_t slice = 0; slice < slices; slice++) {
            int64_t a = a_base + slice;
            int64_t b = b_base + slice;
            rt_mesh3d_add_triangle(m, a, a + 1, b);
            rt_mesh3d_add_triangle(m, a + 1, b + 1, b);
        }
    }
    {
        int64_t last_ring = first_ring + (rings - 2) * ring_stride;
        for (int64_t slice = 0; slice < slices; slice++) {
            int64_t a = last_ring + slice;
            rt_mesh3d_add_triangle(m, a, a + 1, bottom_index);
        }
    }
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)m);
    free(slice_trig);
    free(ring_trig);

    return mesh_return_null_if_build_failed(m);
}

/// @brief Generate a flat plane mesh on the XZ plane (facing +Y) with the given size.
/// @details Emits four vertices and two counter-clockwise triangles centered at the origin, with a
///          full normalized UV rectangle and positive-Y normals.
/// @param sx Positive finite full extent along X.
/// @param sz Positive finite full extent along Z.
/// @return New plane Mesh3D, or NULL for invalid dimensions or allocation/build failure.
void *rt_mesh3d_new_plane(double sx, double sz) {
    if (!mesh_validate_positive_finite(sx, "Mesh3D.NewPlane: sx") ||
        !mesh_validate_positive_finite(sz, "Mesh3D.NewPlane: sz"))
        return NULL;
    void *m = rt_mesh3d_new();
    if (!m)
        return NULL;
    if (!mesh3d_reserve_storage((rt_mesh3d *)m, 4u, 6u, "Mesh3D.NewPlane")) {
        mesh_mark_build_failed((rt_mesh3d *)m);
        return mesh_return_null_if_build_failed(m);
    }

    double hx = sx * 0.5, hz = sz * 0.5;

    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)m);
    rt_mesh3d_add_vertex(m, -hx, 0, -hz, 0, 1, 0, 0, 0);
    rt_mesh3d_add_vertex(m, hx, 0, -hz, 0, 1, 0, 1, 0);
    rt_mesh3d_add_vertex(m, hx, 0, hz, 0, 1, 0, 1, 1);
    rt_mesh3d_add_vertex(m, -hx, 0, hz, 0, 1, 0, 0, 1);

    /* CCW when viewed from +Y so normals, lighting, and backface culling agree. */
    rt_mesh3d_add_triangle(m, 0, 2, 1);
    rt_mesh3d_add_triangle(m, 0, 3, 2);
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)m);

    return mesh_return_null_if_build_failed(m);
}

/// @brief Generate a cylinder mesh with circular caps, centered at the origin.
/// @details Side vertices are seam-duplicated for continuous cylindrical UVs; each cap receives
///          independent vertices with planar UVs and constant axial normals. Segment counts below
///          three are promoted to three and excessive counts trap.
/// @param radius Positive finite cylinder radius.
/// @param height Positive finite full height along Y.
/// @param segments Requested radial segment count.
/// @return New capped cylinder Mesh3D, or NULL for invalid dimensions, excessive complexity, or
///         allocation/build failure.
void *rt_mesh3d_new_cylinder(double radius, double height, int64_t segments) {
    if (!mesh_validate_positive_finite(radius, "Mesh3D.NewCylinder: radius") ||
        !mesh_validate_positive_finite(height, "Mesh3D.NewCylinder: height"))
        return NULL;
    if (segments > MESH_MAX_CYLINDER_SEGMENTS) {
        rt_trap("Mesh3D.NewCylinder: segments exceeds safe limit");
        return NULL;
    }
    void *m = rt_mesh3d_new();
    if (!m)
        return NULL;
    if (segments < 3)
        segments = 3;

    double r = radius;
    double hy = height * 0.5;
    float *trig = NULL;
    if (!mesh3d_check_planned_payload((uint64_t)(4 * segments + 4),
                                      (uint64_t)(12 * segments),
                                      "Mesh3D.NewCylinder: mesh exceeds memory budget")) {
        mesh_mark_build_failed((rt_mesh3d *)m);
        return mesh_return_null_if_build_failed(m);
    }
    if (!mesh3d_reserve_storage((rt_mesh3d *)m,
                                (uint32_t)(4 * segments + 4),
                                (uint32_t)(12 * segments),
                                "Mesh3D.NewCylinder")) {
        mesh_mark_build_failed((rt_mesh3d *)m);
        return mesh_return_null_if_build_failed(m);
    }
    trig = (float *)malloc((size_t)(segments + 1) * 2u * sizeof(float));
    if (!trig) {
        mesh_mark_build_failed((rt_mesh3d *)m);
        rt_trap("Mesh3D.NewCylinder: trigonometry table allocation failed");
        return mesh_return_null_if_build_failed(m);
    }
    for (int64_t i = 0; i <= segments; i++) {
        float theta = 2.0f * (float)M_PI * (float)i / (float)segments;
        trig[(size_t)i * 2u + 0u] = cosf(theta);
        trig[(size_t)i * 2u + 1u] = sinf(theta);
    }

    /* Side vertices */
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)m);
    for (int64_t i = 0; i <= segments; i++) {
        float ct = trig[(size_t)i * 2u + 0u];
        float st = trig[(size_t)i * 2u + 1u];
        float u = (float)i / (float)segments;

        rt_mesh3d_add_vertex(m, r * ct, -hy, r * st, ct, 0, st, u, 1.0);
        rt_mesh3d_add_vertex(m, r * ct, hy, r * st, ct, 0, st, u, 0.0);
    }

    /* Side triangles */
    for (int64_t i = 0; i < segments; i++) {
        int64_t b = i * 2;
        rt_mesh3d_add_triangle(m, b, b + 1, b + 2);
        rt_mesh3d_add_triangle(m, b + 1, b + 3, b + 2);
    }

    /* Top cap center */
    int64_t tc = (int64_t)((rt_mesh3d *)m)->vertex_count;
    rt_mesh3d_add_vertex(m, 0, hy, 0, 0, 1, 0, 0.5, 0.5);
    for (int64_t i = 0; i < segments; i++) {
        float ct = trig[(size_t)i * 2u + 0u];
        float st = trig[(size_t)i * 2u + 1u];
        rt_mesh3d_add_vertex(m, r * ct, hy, r * st, 0, 1, 0, ct * 0.5 + 0.5, st * 0.5 + 0.5);
    }
    for (int64_t i = 0; i < segments; i++) {
        int64_t next = (i + 1) % segments;
        rt_mesh3d_add_triangle(m, tc, tc + 1 + next, tc + 1 + i);
    }

    /* Bottom cap center */
    int64_t bc = (int64_t)((rt_mesh3d *)m)->vertex_count;
    rt_mesh3d_add_vertex(m, 0, -hy, 0, 0, -1, 0, 0.5, 0.5);
    for (int64_t i = 0; i < segments; i++) {
        float ct = trig[(size_t)i * 2u + 0u];
        float st = trig[(size_t)i * 2u + 1u];
        rt_mesh3d_add_vertex(m, r * ct, -hy, r * st, 0, -1, 0, ct * 0.5 + 0.5, st * 0.5 + 0.5);
    }
    for (int64_t i = 0; i < segments; i++) {
        int64_t next = (i + 1) % segments;
        rt_mesh3d_add_triangle(m, bc, bc + 1 + i, bc + 1 + next);
    }
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)m);
    free(trig);

    return mesh_return_null_if_build_failed(m);
}
