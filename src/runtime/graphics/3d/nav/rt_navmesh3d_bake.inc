//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/nav/rt_navmesh3d_bake.inc
// Purpose: Versioned NavMesh3D import/export and the bounded, from-scratch
//   voxel-bake pipeline. This implementation fragment is included by
//   rt_navmesh3d.c and is not a standalone translation unit.
//
// Key invariants:
//   - File counts and allocation products are validated before allocation.
//   - Voxel dimensions are coarsened to fixed dimension and memory budgets.
//   - Imported and baked adjacency/query structures are fully derived before
//     the new object is returned.
//
// Ownership/Lifetime:
//   - Import and bake helpers own partial objects until successful return.
//   - Error paths release all staged arrays through navmesh3d_free_partial.
//
// Links: rt_navmesh3d.c, rt_navmesh3d_build.inc, rt_navmesh3d_query.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_navmesh3d_bake.inc
 * @brief Implements versioned import, bounded voxel baking, surface projection, and off-mesh
 *        adjacency refresh for NavMesh3D.
 *
 * This private implementation fragment is included by rt_navmesh3d.c. It validates serialized
 * counts before allocation, coarsens voxel grids to fixed dimension and memory budgets, publishes
 * fully derived topology, and supplies the horizontal geometry primitives shared by path and
 * nearest-point queries.
 */

/// @brief Parse a VNAVMSH2 navmesh body (after its magic has been consumed) into a fresh
///   NavMesh3D, or NULL on malformed or oversized data.
/// @param f Open binary stream positioned immediately after the eight-byte version-two magic.
/// @return New GC-managed NavMesh3D with rebuilt derived indexes, or null after releasing partial
///         state on malformed input.
static rt_navmesh3d *navmesh3d_import_v2_after_magic(FILE *f) {
    rt_navmesh3d *nm =
        (rt_navmesh3d *)rt_obj_new_i64(RT_G3D_NAVMESH3D_CLASS_ID, (int64_t)sizeof(rt_navmesh3d));
    double agent_radius, agent_height, max_slope, tile_size;
    uint8_t skip_gate = 0;
    int32_t vc = 0, area_count = 0, source_tc = 0, tc = 0, link_count = 0, obstacle_count = 0;
    if (!nm) {
        rt_trap("NavMesh3D.Import: allocation failed");
        return NULL;
    }
    memset(nm, 0, sizeof(*nm));
    rt_obj_set_finalizer(nm, navmesh3d_finalizer);
    if (!navmesh3d_read_f64(f, &agent_radius) || !navmesh3d_read_f64(f, &agent_height) ||
        !navmesh3d_read_f64(f, &max_slope) || !navmesh3d_read_f64(f, &tile_size) ||
        !navmesh3d_read_u8(f, &skip_gate) || !navmesh3d_read_i32(f, &vc) || vc < 0 ||
        vc > NAVMESH3D_IMPORT_MAX_RECORDS)
        goto fail;
    nm->agent_radius = navmesh3d_agent_dim_or(agent_radius, 0.4);
    nm->agent_height = navmesh3d_agent_dim_or(agent_height, 1.8);
    nm->max_slope = navmesh3d_sanitize_slope(max_slope);
    nm->tile_size = (isfinite(tile_size) && tile_size > 0.0 && tile_size <= NAVMESH3D_TILE_SIZE_MAX)
                        ? tile_size
                        : 0.0;
    nm->skip_portal_width_gate = skip_gate ? 1 : 0;
    if (vc > 0) {
        nm->vertices = (nav_vertex_t *)calloc((size_t)vc, sizeof(nav_vertex_t));
        if (!nm->vertices)
            goto fail;
        for (int32_t i = 0; i < vc; i++) {
            if (!navmesh3d_read_f32(f, &nm->vertices[i].position[0]) ||
                !navmesh3d_read_f32(f, &nm->vertices[i].position[1]) ||
                !navmesh3d_read_f32(f, &nm->vertices[i].position[2]))
                goto fail;
        }
    }
    nm->vertex_count = vc;
    if (!navmesh3d_read_i32(f, &area_count) || area_count < 0 ||
        area_count > NAVMESH3D_IMPORT_MAX_RECORDS)
        goto fail;
    if (area_count > 0) {
        nm->area_names = (rt_string *)calloc((size_t)area_count, sizeof(rt_string));
        if (!nm->area_names)
            goto fail;
        nm->area_name_capacity = area_count;
        for (int32_t i = 0; i < area_count; i++) {
            if (!navmesh3d_read_string(f, &nm->area_names[i], NAVMESH3D_IMPORT_MAX_STRING_BYTES))
                goto fail;
            nm->area_name_count = i + 1;
            int64_t current_len = rt_str_len(nm->area_names[i]);
            if (current_len <= 0)
                goto fail;
            const char *current_bytes = rt_string_cstr(nm->area_names[i]);
            if (!current_bytes)
                goto fail;
            for (int32_t prior = 0; prior < i; prior++) {
                int64_t prior_len = rt_str_len(nm->area_names[prior]);
                const char *prior_bytes = rt_string_cstr(nm->area_names[prior]);
                if (prior_len < 0 || !prior_bytes)
                    goto fail;
                if (prior_len == current_len &&
                    memcmp(prior_bytes, current_bytes, (size_t)current_len) == 0)
                    goto fail;
            }
        }
    }
    if (!navmesh3d_read_i32(f, &source_tc) || source_tc < 0 ||
        source_tc > NAVMESH3D_IMPORT_MAX_RECORDS)
        goto fail;
    if (source_tc > 0) {
        nm->source_triangles = (nav_triangle_t *)calloc((size_t)source_tc, sizeof(nav_triangle_t));
        if (!nm->source_triangles)
            goto fail;
        for (int32_t i = 0; i < source_tc; i++) {
            if (!navmesh3d_read_triangle_record(
                    f, nm, &nm->source_triangles[i], nm->area_name_count))
                goto fail;
            nm->source_triangle_count = i + 1;
        }
    }
    if (!navmesh3d_read_i32(f, &tc) || tc < 0 || tc > NAVMESH3D_IMPORT_MAX_RECORDS)
        goto fail;
    if (tc > 0) {
        nm->triangles = (nav_triangle_t *)calloc((size_t)tc, sizeof(nav_triangle_t));
        if (!nm->triangles)
            goto fail;
        for (int32_t i = 0; i < tc; i++) {
            if (!navmesh3d_read_triangle_record(f, nm, &nm->triangles[i], nm->area_name_count))
                goto fail;
            nm->triangle_count = i + 1;
        }
    }
    if (!nm->source_triangles && tc > 0) {
        nm->source_triangles = (nav_triangle_t *)malloc((size_t)tc * sizeof(nav_triangle_t));
        if (!nm->source_triangles)
            goto fail;
        memcpy(nm->source_triangles, nm->triangles, (size_t)tc * sizeof(nav_triangle_t));
        nm->source_triangle_count = tc;
    }
    if (!navmesh3d_read_i32(f, &link_count) || link_count < 0 ||
        link_count > NAVMESH3D_IMPORT_MAX_RECORDS)
        goto fail;
    if (link_count > 0) {
        nm->offmesh_links =
            (nav_offmesh_link_t *)calloc((size_t)link_count, sizeof(nav_offmesh_link_t));
        if (!nm->offmesh_links)
            goto fail;
        nm->offmesh_link_capacity = link_count;
        for (int32_t i = 0; i < link_count; i++) {
            nav_offmesh_link_t *link = &nm->offmesh_links[i];
            uint8_t bidirectional = 0;
            rt_string kind = NULL;
            if (!navmesh3d_read_f32(f, &link->from[0]) || !navmesh3d_read_f32(f, &link->from[1]) ||
                !navmesh3d_read_f32(f, &link->from[2]) || !navmesh3d_read_f32(f, &link->to[0]) ||
                !navmesh3d_read_f32(f, &link->to[1]) || !navmesh3d_read_f32(f, &link->to[2]) ||
                !navmesh3d_read_u8(f, &bidirectional) ||
                !navmesh3d_read_i64(f, &link->state_flags) ||
                !navmesh3d_read_f32(f, &link->traversal_cost) ||
                !navmesh3d_read_string(f, &kind, NAVMESH3D_IMPORT_MAX_STRING_BYTES))
                goto fail;
            link->bidirectional = bidirectional ? 1 : 0;
            link->from_tri = -1;
            link->to_tri = -1;
            link->traversal_cost = navmesh3d_sanitize_traversal_cost((double)link->traversal_cost);
            if (kind && rt_str_len(kind) > 0)
                link->kind = kind;
            else if (kind)
                rt_string_unref(kind);
            nm->offmesh_link_count = i + 1;
        }
    }
    if (!navmesh3d_read_i32(f, &obstacle_count) || obstacle_count < 0 ||
        obstacle_count > NAVMESH3D_IMPORT_MAX_RECORDS)
        goto fail;
    if (obstacle_count > 0) {
        nm->obstacles = (nav_obstacle_t *)calloc((size_t)obstacle_count, sizeof(nav_obstacle_t));
        if (!nm->obstacles)
            goto fail;
        nm->obstacle_capacity = obstacle_count;
        for (int32_t i = 0; i < obstacle_count; i++) {
            if (!navmesh3d_read_f32(f, &nm->obstacles[i].min[0]) ||
                !navmesh3d_read_f32(f, &nm->obstacles[i].min[1]) ||
                !navmesh3d_read_f32(f, &nm->obstacles[i].min[2]) ||
                !navmesh3d_read_f32(f, &nm->obstacles[i].max[0]) ||
                !navmesh3d_read_f32(f, &nm->obstacles[i].max[1]) ||
                !navmesh3d_read_f32(f, &nm->obstacles[i].max[2]))
                goto fail;
            for (int lane = 0; lane < 3; lane++) {
                if (nm->obstacles[i].min[lane] > nm->obstacles[i].max[lane]) {
                    float swap = nm->obstacles[i].min[lane];
                    nm->obstacles[i].min[lane] = nm->obstacles[i].max[lane];
                    nm->obstacles[i].max[lane] = swap;
                }
            }
            nm->obstacle_count = i + 1;
        }
    }
    if (fgetc(f) != EOF)
        goto fail;
    if (!navmesh3d_build_adjacency(nm))
        goto fail;
    navmesh3d_refresh_offmesh_links(nm);
    (void)navmesh3d_build_query_grid(nm);
    nm->topology_revision = 1u;
    return nm;

fail:
    navmesh3d_free_partial(nm);
    return NULL;
}

/// @brief Reconstruct a navmesh from "VNAVMSH2" or legacy "VNAVMSH1" files.
/// @details A missing/short/corrupt/trailing-data file returns NULL recoverably; allocation
///   failure traps. Imported meshes rebuild adjacency and point-location grids immediately.
/// @param path Runtime string containing the UTF-8 source filename.
/// @return New GC-managed path-queryable NavMesh3D, or null for an invalid, missing, or corrupt
///         file.
void *rt_navmesh3d_import(rt_string path) {
    if (!path || !rt_string_is_handle(path))
        return NULL;
    const char *cpath = rt_string_cstr(path);
    if (!cpath || cpath[0] == '\0')
        return NULL;
    FILE *f = rt_file_stdio_open_utf8(cpath, "rb");
    if (!f)
        return NULL;
    char magic[8];
    rt_navmesh3d *result = NULL;
    if (fread(magic, 1u, 8u, f) == 8u) {
        if (memcmp(magic, "VNAVMSH2", 8u) == 0)
            result = navmesh3d_import_v2_after_magic(f);
        else if (memcmp(magic, "VNAVMSH1", 8u) == 0)
            result = navmesh3d_import_v1_after_magic(f);
    }
    fclose(f);
    return result;
}

/// @brief Bake a navmesh from all Mesh3D-bearing nodes in a Scene3D.
/// @details Flattens scene node meshes through their world transforms into a temporary world-space
/// Mesh3D, then runs the from-scratch voxel baker (`navmesh3d_voxel_bake`): the walkable surface is
/// rasterized into a `cell_size` heightfield grid, eroded by the agent radius, and re-emitted as a
/// shared-corner grid mesh. Unlike `Build`, the result is decoupled from the input triangle density
/// and honors the agent radius, which is what `Scene3D`/`World3D` auto-bake expects.
/// @param scene Borrowed Scene3D whose mesh nodes are collected in world space.
/// @param agent_radius Requested non-negative clearance radius.
/// @param agent_height Requested non-negative agent height.
/// @param max_slope Maximum walkable slope in degrees.
/// @param cell_size Requested voxel cell edge length.
/// @return New GC-managed baked NavMesh3D, or null when geometry collection or voxel baking fails.
void *rt_navmesh3d_bake(
    void *scene, double agent_radius, double agent_height, double max_slope, double cell_size) {
    void *source_mesh = navmesh3d_build_scene_source_mesh(scene);
    rt_navmesh3d *nm;
    if (!source_mesh)
        return NULL;
    nm = (rt_navmesh3d *)navmesh3d_voxel_bake(
        (rt_mesh3d *)source_mesh, agent_radius, agent_height, max_slope, cell_size);
    navmesh3d_release_local(source_mesh);

    /* Every bake is tiled: derive a default tile size (~1/16th of the larger
     * XZ extent, clamped) so RebuildTile/AddObstacle always take the O(tile)
     * in-place re-carve path — destructibles never pay a whole-mesh refilter.
     * BakeTiled still overrides with an explicit tile size. */
    if (nm && nm->tile_size <= 0.0 && nm->qgrid_nx > 0 && nm->qgrid_nz > 0 &&
        nm->qgrid_inv_cell > 0.0) {
        double cell = 1.0 / nm->qgrid_inv_cell;
        double extent_x = (double)nm->qgrid_nx * cell;
        double extent_z = (double)nm->qgrid_nz * cell;
        double extent = extent_x > extent_z ? extent_x : extent_z;
        double tile = extent / 16.0;
        if (tile < 4.0)
            tile = 4.0;
        if (tile > NAVMESH3D_TILE_SIZE_MAX)
            tile = NAVMESH3D_TILE_SIZE_MAX;
        if (tile < NAVMESH3D_CELL_SIZE_MIN)
            tile = NAVMESH3D_CELL_SIZE_MIN;
        nm->tile_size = tile;
    }
    return nm;
}

/// @brief Bake a full-scene navmesh and tag it tiled so obstacle edits can rebuild per tile.
/// @details The triangle set is still baked over the whole scene (one voxel pass), but recording
/// `tile_size` switches `RebuildTile` and `AddObstacle` onto the O(tile) in-place re-carve path
/// instead of a whole-mesh refilter. Tile (tx,tz) covers world XZ
/// [meshMin + t*tile_size, meshMin + (t+1)*tile_size). A non-positive tile_size leaves the mesh
/// untiled (whole-mesh refilter), matching `Bake`.
/// @param scene Borrowed Scene3D whose mesh nodes are collected.
/// @param tile_size Requested tile edge length, sanitized to the supported range.
/// @param agent_radius Requested non-negative clearance radius.
/// @param agent_height Requested non-negative agent height.
/// @param max_slope Maximum walkable slope in degrees.
/// @param cell_size Requested voxel cell edge length.
/// @return New GC-managed NavMesh3D tagged for tile-local mutation, or null on bake failure.
void *rt_navmesh3d_bake_tiled(void *scene,
                              double tile_size,
                              double agent_radius,
                              double agent_height,
                              double max_slope,
                              double cell_size) {
    void *obj = rt_navmesh3d_bake(scene, agent_radius, agent_height, max_slope, cell_size);
    if (!isfinite(tile_size) || tile_size <= 0.0)
        tile_size = 0.0;
    else {
        if (tile_size < NAVMESH3D_CELL_SIZE_MIN)
            tile_size = NAVMESH3D_CELL_SIZE_MIN;
        if (tile_size > NAVMESH3D_TILE_SIZE_MAX)
            tile_size = NAVMESH3D_TILE_SIZE_MAX;
    }
    if (obj && tile_size > 0.0) {
        rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
        if (nm)
            nm->tile_size = tile_size;
    }
    return obj;
}

/// @brief Point-in-triangle test on XZ plane (2D barycentric).
/// @param px Query X coordinate.
/// @param pz Query Z coordinate.
/// @param v0 Borrowed first triangle vertex.
/// @param v1 Borrowed second triangle vertex.
/// @param v2 Borrowed third triangle vertex.
/// @return 1 when the finite query lies inside or on the triangle within tolerance, otherwise 0.
static int point_in_tri_xz(float px, float pz, const float *v0, const float *v1, const float *v2) {
    if (!isfinite(px) || !isfinite(pz) || !navmesh3d_float3_finite(v0) ||
        !navmesh3d_float3_finite(v1) || !navmesh3d_float3_finite(v2))
        return 0;
    float d1x = v1[0] - v0[0], d1z = v1[2] - v0[2];
    float d2x = v2[0] - v0[0], d2z = v2[2] - v0[2];
    float dpx = px - v0[0], dpz = pz - v0[2];
    const float eps = 1e-5f;

    float det = d1x * d2z - d2x * d1z;
    if (!isfinite(det) || fabsf(det) < 1e-8f)
        return 0;
    float inv = 1.0f / det;
    float u = (dpx * d2z - d2x * dpz) * inv;
    float v = (d1x * dpz - dpx * d1z) * inv;
    return (u >= -eps && v >= -eps && (u + v) <= 1.0f + eps) ? 1 : 0;
}

/// @brief Interpolate the world-space Y coordinate at `(px, pz)` on a triangle defined by
///        @p v0 / @p v1 / @p v2. Used by navmesh height queries to project an XZ point onto
///        the walkable surface. Caller must already have confirmed `(px,pz)` lies inside.
/// @param px Query X coordinate.
/// @param pz Query Z coordinate.
/// @param v0 Borrowed first triangle vertex.
/// @param v1 Borrowed second triangle vertex.
/// @param v2 Borrowed third triangle vertex.
/// @return Finite interpolated height, or a sanitized centroid-height fallback for a degenerate
///         projection.
static float triangle_y_at_xz(
    float px, float pz, const float *v0, const float *v1, const float *v2) {
    if (!isfinite(px) || !isfinite(pz) || !navmesh3d_float3_finite(v0) ||
        !navmesh3d_float3_finite(v1) || !navmesh3d_float3_finite(v2))
        return 0.0f;
    float d1x = v1[0] - v0[0], d1z = v1[2] - v0[2];
    float d2x = v2[0] - v0[0], d2z = v2[2] - v0[2];
    float dpx = px - v0[0], dpz = pz - v0[2];
    float det = d1x * d2z - d2x * d1z;
    if (!isfinite(det) || fabsf(det) < 1e-8f)
        return navmesh3d_coordf_or(((double)v0[1] + (double)v1[1] + (double)v2[1]) / 3.0, 0.0);
    float inv = 1.0f / det;
    float u = (dpx * d2z - d2x * dpz) * inv;
    float v = (d1x * dpz - dpx * d1z) * inv;
    return navmesh3d_coordf_or(v0[1] + u * (v1[1] - v0[1]) + v * (v2[1] - v0[1]), 0.0);
}

/// @brief Get-or-create the shared grid-corner vertex at corner `(ccx, ccz)` in the navmesh vertex
///   array, placing it at the average surface height of the up-to-four walkable cells touching the
///   corner. Sharing one vertex per corner is what makes adjacent walkable cells share edge
///   vertices, so `navmesh3d_build_adjacency` connects them. Returns the vertex index.
/// @param nm Mutable navmesh receiving a newly materialized corner vertex when needed.
/// @param corner_vid Mutable `(nx + 1) * (nz + 1)` corner-to-vertex lookup.
/// @param cell_h Borrowed per-cell surface heights.
/// @param cell_walk Borrowed per-cell walkability flags.
/// @param nx Grid width in cells.
/// @param nz Grid depth in cells.
/// @param min_x World X coordinate of the grid origin.
/// @param min_z World Z coordinate of the grid origin.
/// @param cell_size Positive grid cell edge length.
/// @param ccx X coordinate of the requested grid corner.
/// @param ccz Z coordinate of the requested grid corner.
/// @return Existing or newly appended navmesh vertex index for the corner.
static int32_t navmesh3d_voxel_corner_vertex(rt_navmesh3d *nm,
                                             int32_t *corner_vid,
                                             const float *cell_h,
                                             const uint8_t *cell_walk,
                                             int32_t nx,
                                             int32_t nz,
                                             double min_x,
                                             double min_z,
                                             double cell_size,
                                             int32_t ccx,
                                             int32_t ccz) {
    int32_t cnx = nx + 1;
    size_t cidx = (size_t)ccz * (size_t)cnx + (size_t)ccx;
    if (corner_vid[cidx] >= 0)
        return corner_vid[cidx];
    double sum = 0.0;
    int n = 0;
    for (int dz = -1; dz <= 0; dz++) {
        for (int dx = -1; dx <= 0; dx++) {
            int32_t sx = ccx + dx;
            int32_t sz = ccz + dz;
            if (sx < 0 || sz < 0 || sx >= nx || sz >= nz)
                continue;
            size_t s = (size_t)sz * (size_t)nx + (size_t)sx;
            if (!cell_walk[s])
                continue;
            sum += (double)cell_h[s];
            n++;
        }
    }
    int32_t vid = nm->vertex_count++;
    nm->vertices[vid].position[0] = (float)(min_x + (double)ccx * cell_size);
    nm->vertices[vid].position[1] = (n > 0) ? (float)(sum / (double)n) : 0.0f;
    nm->vertices[vid].position[2] = (float)(min_z + (double)ccz * cell_size);
    corner_vid[cidx] = vid;
    return vid;
}

/// @brief Append one walkable-surface triangle (indices into the navmesh vertex array) to the
///   navmesh source-triangle set, computing an up-facing face normal and centroid. Winding does
///   not matter to the downstream point-in-triangle / A* queries; the normal is forced to +Y so the
///   slope filter retains the surface.
/// @param nm Mutable navmesh with preallocated vertex and source-triangle storage.
/// @param a First vertex index.
/// @param b Second vertex index.
/// @param c Third vertex index.
/// @param blocked Non-zero to mark the emitted source triangle unavailable to queries.
static void navmesh3d_voxel_emit_tri(
    rt_navmesh3d *nm, int32_t a, int32_t b, int32_t c, int8_t blocked) {
    const float *p0 = nm->vertices[a].position;
    const float *p1 = nm->vertices[b].position;
    const float *p2 = nm->vertices[c].position;
    if (!navmesh3d_float3_finite(p0) || !navmesh3d_float3_finite(p1) ||
        !navmesh3d_float3_finite(p2))
        return;
    float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
    float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
    float nx_ = e1y * e2z - e1z * e2y;
    float ny_ = e1z * e2x - e1x * e2z;
    float nz_ = e1x * e2y - e1y * e2x;
    float nlen = sqrtf(nx_ * nx_ + ny_ * ny_ + nz_ * nz_);
    if (!isfinite(nlen) || nlen < 1e-8f) {
        nx_ = 0.0f;
        ny_ = 1.0f;
        nz_ = 0.0f;
        nlen = 1.0f;
    }
    nx_ /= nlen;
    ny_ /= nlen;
    nz_ /= nlen;
    if (ny_ < 0.0f) {
        nx_ = -nx_;
        ny_ = -ny_;
        nz_ = -nz_;
    }
    nav_triangle_t *tri = &nm->source_triangles[nm->source_triangle_count++];
    tri->v[0] = a;
    tri->v[1] = b;
    tri->v[2] = c;
    tri->normal[0] = nx_;
    tri->normal[1] = ny_;
    tri->normal[2] = nz_;
    tri->centroid[0] = (p0[0] + p1[0] + p2[0]) / 3.0f;
    tri->centroid[1] = (p0[1] + p1[1] + p2[1]) / 3.0f;
    tri->centroid[2] = (p0[2] + p1[2] + p2[2]) / 3.0f;
    tri->neighbors[0] = tri->neighbors[1] = tri->neighbors[2] = -1;
    tri->blocked = blocked ? 1 : 0;
    tri->area_id = 0;
    tri->traversal_cost = 1.0f;
}

/// @brief Rasterize slope-walkable source triangles into the voxel grid, keeping the highest
///        walkable surface height per cell. Fills @p cell_h / @p cell_walk (caller-allocated).
/// @param src Borrowed source mesh containing indexed world-space triangles.
/// @param min_x World X coordinate of the voxel-grid origin.
/// @param min_z World Z coordinate of the voxel-grid origin.
/// @param cell_size Positive grid cell edge length.
/// @param nx Grid width in cells.
/// @param nz Grid depth in cells.
/// @param walkable_cos Minimum absolute face-normal Y component accepted as walkable.
/// @param cell_h Caller-owned per-cell height buffer.
/// @param cell_walk Caller-owned per-cell walkability buffer.
static void navmesh3d_voxel_rasterize(rt_mesh3d *src,
                                      double min_x,
                                      double min_z,
                                      double cell_size,
                                      int32_t nx,
                                      int32_t nz,
                                      double walkable_cos,
                                      float *cell_h,
                                      uint8_t *cell_walk) {
    for (uint32_t t = 0; t + 2 < src->index_count; t += 3) {
        uint32_t i0 = src->indices[t], i1 = src->indices[t + 1], i2 = src->indices[t + 2];
        if (i0 >= src->vertex_count || i1 >= src->vertex_count || i2 >= src->vertex_count)
            continue;
        const float *p0 = src->vertices[i0].pos;
        const float *p1 = src->vertices[i1].pos;
        const float *p2 = src->vertices[i2].pos;
        if (!navmesh3d_float3_finite(p0) || !navmesh3d_float3_finite(p1) ||
            !navmesh3d_float3_finite(p2))
            continue;
        float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
        float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
        float fnx = e1y * e2z - e1z * e2y;
        float fny = e1z * e2x - e1x * e2z;
        float fnz = e1x * e2y - e1y * e2x;
        float flen = sqrtf(fnx * fnx + fny * fny + fnz * fnz);
        if (!isfinite(flen) || flen < 1e-8f)
            continue;
        if (fabsf(fny / flen) < (float)walkable_cos)
            continue; /* face too steep to walk */
        float tminx = fminf(p0[0], fminf(p1[0], p2[0]));
        float tmaxx = fmaxf(p0[0], fmaxf(p1[0], p2[0]));
        float tminz = fminf(p0[2], fminf(p1[2], p2[2]));
        float tmaxz = fmaxf(p0[2], fmaxf(p1[2], p2[2]));
        if (!isfinite(tminx) || !isfinite(tmaxx) || !isfinite(tminz) || !isfinite(tmaxz))
            continue;
        int cx0 = (int)((tminx - min_x) / cell_size);
        int cx1 = (int)((tmaxx - min_x) / cell_size);
        int cz0 = (int)((tminz - min_z) / cell_size);
        int cz1 = (int)((tmaxz - min_z) / cell_size);
        if (cx0 < 0)
            cx0 = 0;
        if (cz0 < 0)
            cz0 = 0;
        if (cx1 >= nx)
            cx1 = nx - 1;
        if (cz1 >= nz)
            cz1 = nz - 1;
        for (int cz = cz0; cz <= cz1; cz++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                float px = (float)(min_x + ((double)cx + 0.5) * cell_size);
                float pz = (float)(min_z + ((double)cz + 0.5) * cell_size);
                if (!point_in_tri_xz(px, pz, p0, p1, p2))
                    continue;
                float y = triangle_y_at_xz(px, pz, p0, p1, p2);
                size_t idx = (size_t)cz * (size_t)nx + (size_t)cx;
                if (!cell_walk[idx] || y > cell_h[idx]) {
                    cell_h[idx] = y;
                    cell_walk[idx] = 1;
                }
            }
        }
    }
}

/// @brief Erode the walkable set by @p erode_r cells; grid edges count as boundaries so the
///        navmesh insets by the agent radius.
/// @param cell_walk Borrowed `nx * nz` source walkability flags.
/// @param nx Grid width in cells.
/// @param nz Grid depth in cells.
/// @param erode_r Non-negative square-neighborhood erosion radius in cells.
/// @return Newly allocated eroded walkability buffer owned by the caller, or null on allocation
///         failure.
static uint8_t *navmesh3d_voxel_erode(const uint8_t *cell_walk,
                                      int32_t nx,
                                      int32_t nz,
                                      int erode_r) {
    size_t cell_count = (size_t)nx * (size_t)nz;
    uint8_t *eroded = (uint8_t *)malloc(cell_count);
    if (!eroded)
        return NULL;
    memcpy(eroded, cell_walk, cell_count);
    for (int32_t cz = 0; cz < nz; cz++) {
        for (int32_t cx = 0; cx < nx; cx++) {
            size_t idx = (size_t)cz * (size_t)nx + (size_t)cx;
            if (!cell_walk[idx])
                continue;
            int blocked = 0;
            for (int dz = -erode_r; dz <= erode_r && !blocked; dz++) {
                for (int dx = -erode_r; dx <= erode_r; dx++) {
                    int sx = cx + dx, sz = cz + dz;
                    if (sx < 0 || sz < 0 || sx >= nx || sz >= nz) {
                        blocked = 1;
                        break;
                    }
                    if (!cell_walk[(size_t)sz * (size_t)nx + (size_t)sx]) {
                        blocked = 1;
                        break;
                    }
                }
            }
            if (blocked)
                eroded[idx] = 0;
        }
    }
    return eroded;
}

/// @brief Emit two triangles per grid cell into @p nm, sharing corner vertices via @p corner_vid.
/// @param nm Mutable navmesh receiving vertices and source triangles.
/// @param corner_vid Mutable shared-corner vertex lookup.
/// @param cell_h Borrowed per-cell height array.
/// @param cell_walk Borrowed per-cell walkability array.
/// @param nx Grid width in cells.
/// @param nz Grid depth in cells.
/// @param min_x World X coordinate of the grid origin.
/// @param min_z World Z coordinate of the grid origin.
/// @param cell_size Positive grid cell edge length.
static void navmesh3d_voxel_emit_grid_mesh(rt_navmesh3d *nm,
                                           int32_t *corner_vid,
                                           float *cell_h,
                                           uint8_t *cell_walk,
                                           int32_t nx,
                                           int32_t nz,
                                           double min_x,
                                           double min_z,
                                           double cell_size) {
    for (int32_t cz = 0; cz < nz; cz++) {
        for (int32_t cx = 0; cx < nx; cx++) {
            size_t cell_idx = (size_t)cz * (size_t)nx + (size_t)cx;
            int8_t blocked = cell_walk[cell_idx] ? 0 : 1;
            int32_t a = navmesh3d_voxel_corner_vertex(
                nm, corner_vid, cell_h, cell_walk, nx, nz, min_x, min_z, cell_size, cx, cz);
            int32_t b = navmesh3d_voxel_corner_vertex(
                nm, corner_vid, cell_h, cell_walk, nx, nz, min_x, min_z, cell_size, cx + 1, cz);
            int32_t c = navmesh3d_voxel_corner_vertex(
                nm, corner_vid, cell_h, cell_walk, nx, nz, min_x, min_z, cell_size, cx + 1, cz + 1);
            int32_t d = navmesh3d_voxel_corner_vertex(
                nm, corner_vid, cell_h, cell_walk, nx, nz, min_x, min_z, cell_size, cx, cz + 1);
            navmesh3d_voxel_emit_tri(nm, a, c, b, blocked);
            navmesh3d_voxel_emit_tri(nm, a, d, c, blocked);
        }
    }
}

/// @brief Number of voxel cells spanning @p span at @p cell_size (floor(span/cell)+1).
/// @param span Finite non-negative world-space extent.
/// @param cell_size Finite positive cell edge length.
/// @param out_dim Receives the representable positive dimension.
/// @return 1 with @p out_dim set, or 0 if the result is non-finite or exceeds INT32_MAX.
static int navmesh3d_voxel_dim_for_span(double span, double cell_size, int64_t *out_dim) {
    if (!out_dim || !isfinite(span) || span < 0.0 || !isfinite(cell_size) || cell_size <= 0.0)
        return 0;
    double dim = floor(span / cell_size) + 1.0;
    if (!isfinite(dim) || dim < 1.0 || dim > (double)INT32_MAX)
        return 0;
    *out_dim = (int64_t)dim;
    return 1;
}

/// @brief Compute the XZ bounding box of a source mesh (finite vertices only).
/// @param src Borrowed source mesh.
/// @param out_min_x Receives the minimum finite X coordinate.
/// @param out_min_z Receives the minimum finite Z coordinate.
/// @param out_max_x Receives the maximum finite X coordinate.
/// @param out_max_z Receives the maximum finite Z coordinate.
/// @return 0 when there are no finite vertices or the bounds are degenerate/non-finite.
static int navmesh3d_voxel_xz_bounds(const rt_mesh3d *src,
                                     double *out_min_x,
                                     double *out_min_z,
                                     double *out_max_x,
                                     double *out_max_z) {
    double min_x = DBL_MAX, min_z = DBL_MAX, max_x = -DBL_MAX, max_z = -DBL_MAX;
    int finite_vertices = 0;
    for (uint32_t i = 0; i < src->vertex_count; i++) {
        if (!navmesh3d_float3_finite(src->vertices[i].pos))
            continue;
        double x = navmesh3d_coord_or((double)src->vertices[i].pos[0], 0.0);
        double z = navmesh3d_coord_or((double)src->vertices[i].pos[2], 0.0);
        finite_vertices++;
        if (x < min_x)
            min_x = x;
        if (x > max_x)
            max_x = x;
        if (z < min_z)
            min_z = z;
        if (z > max_z)
            max_z = z;
    }
    if (finite_vertices == 0 || !(max_x >= min_x) || !(max_z >= min_z) || !isfinite(min_x) ||
        !isfinite(min_z) || !isfinite(max_x) || !isfinite(max_z))
        return 0;
    *out_min_x = min_x;
    *out_min_z = min_z;
    *out_max_x = max_x;
    *out_max_z = max_z;
    return 1;
}

/// @brief Compute the maximum retained voxel-cell count allowed by the memory budget.
/// @details A retained voxel cell owns two source triangles, two live triangles, amortized corner
///   vertex storage, height/walkability, and corner-index data. Deriving the count from the actual
///   C struct sizes prevents silent budget drift when nav-triangle metadata grows.
/// @return Maximum retained voxel-cell count constrained by the configured memory target and
///         absolute cell limits.
static int64_t navmesh3d_voxel_cell_budget(void) {
    size_t bytes_per_cell = 4u * sizeof(nav_triangle_t) + sizeof(nav_vertex_t) + sizeof(float) +
                            sizeof(uint8_t) + sizeof(int32_t);
    if (bytes_per_cell == 0)
        return NAVMESH3D_VOXEL_MIN_CELLS;
    size_t budget = (size_t)NAVMESH3D_VOXEL_TARGET_BYTES / bytes_per_cell;
    if (budget < (size_t)NAVMESH3D_VOXEL_MIN_CELLS)
        budget = (size_t)NAVMESH3D_VOXEL_MIN_CELLS;
    if (budget > (size_t)NAVMESH3D_VOXEL_ABS_MAX_CELLS)
        budget = (size_t)NAVMESH3D_VOXEL_ABS_MAX_CELLS;
    return (int64_t)budget;
}

/// @brief Derive bounded voxel grid dimensions for the given spans, coarsening @p cell_size
///        (in/out) until the grid fits the configured dimension and memory budgets.
/// @param span_x Finite non-negative X extent.
/// @param span_z Finite non-negative Z extent.
/// @param cell_size In/out positive cell size, increased as required to meet budgets.
/// @param out_nx Receives the bounded X dimension.
/// @param out_nz Receives the bounded Z dimension.
/// @return 1 on success, otherwise 0 for invalid or unrepresentable input.
static int navmesh3d_voxel_grid_dims(
    double span_x, double span_z, double *cell_size, int32_t *out_nx, int32_t *out_nz) {
    const int64_t max_dim = NAVMESH3D_VOXEL_MAX_DIM;
    const int64_t max_cells = navmesh3d_voxel_cell_budget();
    double effective_cell;
    double required_x;
    double required_z;
    if (!cell_size || !out_nx || !out_nz || !isfinite(span_x) || !isfinite(span_z) ||
        span_x < 0.0 || span_z < 0.0 || !isfinite(*cell_size) || *cell_size <= 0.0 ||
        max_dim <= 1 || max_cells <= 0)
        return 0;
    effective_cell = *cell_size;
    required_x = span_x / (double)(max_dim - 1);
    required_z = span_z / (double)(max_dim - 1);
    if (!isfinite(required_x) || !isfinite(required_z))
        return 0;
    if (effective_cell < required_x)
        effective_cell = required_x;
    if (effective_cell < required_z)
        effective_cell = required_z;
    if (!(effective_cell > 0.0) || !isfinite(effective_cell))
        return 0;
    for (int guard = 0; guard < 64; guard++) {
        int64_t tnx = 0;
        int64_t tnz = 0;
        if (navmesh3d_voxel_dim_for_span(span_x, effective_cell, &tnx) &&
            navmesh3d_voxel_dim_for_span(span_z, effective_cell, &tnz) && tnx <= max_dim &&
            tnz <= max_dim && tnz > 0 && tnx <= max_cells / tnz)
            break;
        if (effective_cell > DBL_MAX * 0.5)
            return 0;
        effective_cell *= 2.0;
    }
    int64_t nx64 = 0;
    int64_t nz64 = 0;
    if (!navmesh3d_voxel_dim_for_span(span_x, effective_cell, &nx64) ||
        !navmesh3d_voxel_dim_for_span(span_z, effective_cell, &nz64))
        return 0;
    if (nx64 > max_dim || nz64 > max_dim || nz64 <= 0 || nx64 > max_cells / nz64)
        return 0;
    int32_t nx = (int32_t)nx64;
    int32_t nz = (int32_t)nz64;
    if (nx < 1)
        nx = 1;
    *cell_size = effective_cell;
    *out_nx = nx;
    *out_nz = nz;
    return 1;
}

/// @brief Derive bounded voxel-grid dimensions for arithmetic regression tests.
/// @details This test-only entry point applies the same cell-size sanitization and checked sizing
/// as
///   a production bake without allocating a heightfield. It allows extreme finite spans and tiny
///   requested cells to prove that dimension/product calculations neither narrow nor wrap. The
///   function is intentionally absent from the scripting surface.
/// @param span_x Nonnegative finite X extent.
/// @param span_z Nonnegative finite Z extent.
/// @param requested_cell_size Requested cell size; invalid or sub-minimum values are sanitized.
/// @param out_nx Receives the bounded X cell count on success.
/// @param out_nz Receives the bounded Z cell count on success.
/// @param out_cell_count Receives the checked `nx * nz` product on success.
/// @param out_effective_cell_size Receives the possibly coarsened cell size on success.
/// @return 1 when every output is written, or 0 for invalid inputs or an unrepresentable grid.
int8_t rt_navmesh3d_test_voxel_grid_dimensions(double span_x,
                                               double span_z,
                                               double requested_cell_size,
                                               int32_t *out_nx,
                                               int32_t *out_nz,
                                               uint64_t *out_cell_count,
                                               double *out_effective_cell_size) {
    int32_t nx;
    int32_t nz;
    double cell_size;
    uint64_t cells;
    if (!out_nx || !out_nz || !out_cell_count || !out_effective_cell_size || !isfinite(span_x) ||
        !isfinite(span_z) || span_x < 0.0 || span_z < 0.0)
        return 0;
    cell_size = navmesh3d_cell_size_or(requested_cell_size, 0.5);
    if (!navmesh3d_voxel_grid_dims(span_x, span_z, &cell_size, &nx, &nz))
        return 0;
    cells = (uint64_t)(uint32_t)nx * (uint64_t)(uint32_t)nz;
    if (cells == 0u || cells > (uint64_t)navmesh3d_voxel_cell_budget())
        return 0;
    *out_nx = nx;
    *out_nz = nz;
    *out_cell_count = cells;
    *out_effective_cell_size = cell_size;
    return 1;
}

/// @brief Bake a navmesh from arbitrary source geometry via voxelization: rasterize the
///   triangles into an XZ heightfield at @p cell_size, keep cells walkable for an agent of
///   @p agent_radius / @p agent_height within @p max_slope, and emit the walkable nav polygons.
/// @param src Borrowed indexed source mesh in world space.
/// @param agent_radius Requested non-negative clearance radius used for erosion.
/// @param agent_height Requested non-negative agent height retained for vertical queries.
/// @param max_slope Maximum walkable source-triangle slope in degrees.
/// @param cell_size Requested voxel cell edge length, coarsened to fit fixed budgets.
/// @return A new navmesh object, or NULL for empty/degenerate input.
static void *navmesh3d_voxel_bake(
    rt_mesh3d *src, double agent_radius, double agent_height, double max_slope, double cell_size) {
    if (!src || src->vertex_count == 0 || src->index_count < 3)
        return NULL;

    cell_size = navmesh3d_cell_size_or(cell_size, 0.5);
    double radius = navmesh3d_agent_dim_or(agent_radius, 0.4);
    double height = navmesh3d_agent_dim_or(agent_height, 1.8);
    double slope = navmesh3d_sanitize_slope(max_slope);
    double walkable_cos = cos(slope * M_PI / 180.0);

    /* 1. XZ bounds of the world-space source geometry. */
    double min_x, min_z, max_x, max_z;
    if (!navmesh3d_voxel_xz_bounds(src, &min_x, &min_z, &max_x, &max_z))
        return NULL;

    /* 2. Grid dimensions; coarsen cell_size until the grid fits a bounded budget. A navmesh is
     *    coarse relative to render geometry, so a multi-km world collapses into one coarse navmesh
     *    here; fine-grained large-world nav is the tiled baker (R-NAV-002). */
    int32_t nx, nz;
    if (!navmesh3d_voxel_grid_dims(max_x - min_x, max_z - min_z, &cell_size, &nx, &nz))
        return NULL;

    if ((size_t)nx > SIZE_MAX / (size_t)nz)
        return NULL;
    size_t cell_count = (size_t)nx * (size_t)nz;
    if (cell_count > SIZE_MAX / sizeof(float))
        return NULL;
    float *cell_h = (float *)malloc(cell_count * sizeof(float));
    uint8_t *cell_walk = (uint8_t *)calloc(cell_count, 1);
    if (!cell_h || !cell_walk) {
        free(cell_h);
        free(cell_walk);
        return NULL;
    }

    /* 3. Rasterize slope-walkable surfaces into the grid, keeping the highest surface per cell. */
    navmesh3d_voxel_rasterize(
        src, min_x, min_z, cell_size, nx, nz, walkable_cos, cell_h, cell_walk);

    /* 4. Erode the walkable set by the agent radius; the grid edge counts as a boundary so the
     *    navmesh is inset by the radius and corridors narrower than 2*radius drop out. */
    int erode_r = (int)ceil(radius / cell_size);
    if (erode_r > 0) {
        uint8_t *eroded = navmesh3d_voxel_erode(cell_walk, nx, nz, erode_r);
        if (!eroded) {
            free(cell_h);
            free(cell_walk);
            return NULL;
        }
        free(cell_walk);
        cell_walk = eroded;
    }

    int64_t walk_cells = 0;
    for (size_t i = 0; i < cell_count; i++)
        if (cell_walk[i])
            walk_cells++;
    if (walk_cells == 0) {
        free(cell_h);
        free(cell_walk);
        return NULL;
    }

    /* 5. Emit a shared-corner grid mesh into source_triangles. */
    int32_t cnx = nx + 1, cnz = nz + 1;
    if (cnx <= 0 || cnz <= 0 || (size_t)cnx > SIZE_MAX / (size_t)cnz) {
        free(cell_h);
        free(cell_walk);
        return NULL;
    }
    size_t corner_count = (size_t)cnx * (size_t)cnz;
    if (corner_count > SIZE_MAX / sizeof(int32_t)) {
        free(cell_h);
        free(cell_walk);
        return NULL;
    }
    int32_t *corner_vid = (int32_t *)malloc(corner_count * sizeof(int32_t));
    if (!corner_vid) {
        free(cell_h);
        free(cell_walk);
        return NULL;
    }
    for (size_t i = 0; i < corner_count; i++)
        corner_vid[i] = -1;

    rt_navmesh3d *nm =
        (rt_navmesh3d *)rt_obj_new_i64(RT_G3D_NAVMESH3D_CLASS_ID, (int64_t)sizeof(rt_navmesh3d));
    if (!nm) {
        free(cell_h);
        free(cell_walk);
        free(corner_vid);
        return NULL;
    }
    memset(nm, 0, sizeof(*nm));
    nm->agent_radius = radius;
    nm->agent_height = height;
    nm->max_slope = slope;
    nm->skip_portal_width_gate = 1; /* clearance comes from the radius erosion in step 4 */
    rt_obj_set_finalizer(nm, navmesh3d_finalizer);

    if (cell_count > (size_t)INT64_MAX / 2u) {
        free(cell_h);
        free(cell_walk);
        free(corner_vid);
        navmesh3d_free_partial(nm);
        return NULL;
    }
    int64_t tri_cap = (int64_t)(cell_count * 2u);
    if (corner_count > SIZE_MAX / sizeof(nav_vertex_t) ||
        (size_t)tri_cap > SIZE_MAX / sizeof(nav_triangle_t) ||
        cell_count > SIZE_MAX / sizeof(float) || corner_count > SIZE_MAX / sizeof(int32_t)) {
        free(cell_h);
        free(cell_walk);
        free(corner_vid);
        navmesh3d_free_partial(nm);
        return NULL;
    }
    nm->vertices = (nav_vertex_t *)malloc(corner_count * sizeof(nav_vertex_t));
    nm->source_triangles = (nav_triangle_t *)malloc((size_t)tri_cap * sizeof(nav_triangle_t));
    nm->triangles = (nav_triangle_t *)malloc((size_t)tri_cap * sizeof(nav_triangle_t));
    nm->voxel_cell_height = (float *)malloc(cell_count * sizeof(float));
    nm->voxel_cell_walkable = (uint8_t *)malloc(cell_count);
    nm->voxel_corner_vertices = (int32_t *)malloc(corner_count * sizeof(int32_t));
    if (!nm->vertices || !nm->source_triangles || !nm->triangles || !nm->voxel_cell_height ||
        !nm->voxel_cell_walkable || !nm->voxel_corner_vertices) {
        free(cell_h);
        free(cell_walk);
        free(corner_vid);
        navmesh3d_free_partial(nm);
        return NULL;
    }
    memcpy(nm->voxel_cell_height, cell_h, cell_count * sizeof(float));
    memcpy(nm->voxel_cell_walkable, cell_walk, cell_count);
    nm->voxel_min_x = min_x;
    nm->voxel_min_z = min_z;
    nm->voxel_cell_size = cell_size;
    nm->voxel_nx = nx;
    nm->voxel_nz = nz;
    nm->vertex_count = 0;
    nm->source_triangle_count = 0;
    nm->triangle_count = 0;

    navmesh3d_voxel_emit_grid_mesh(
        nm, corner_vid, cell_h, cell_walk, nx, nz, min_x, min_z, cell_size);
    memcpy(nm->voxel_corner_vertices, corner_vid, corner_count * sizeof(int32_t));

    free(cell_h);
    free(cell_walk);
    free(corner_vid);

    /* 6. Filter (keeps all up-facing surfaces), build edge adjacency, refresh off-mesh links. */
    if (!navmesh3d_apply_slope_filter(nm)) {
        navmesh3d_free_partial(nm);
        return NULL;
    }
    return nm;
}

/// @brief Vertical snap tolerance for point-on-mesh tests: half the agent height,
///        floored at 0.25 so a zero/degenerate agent height still tolerates small
///        Y gaps between the query point and a triangle.
/// @param nm Optional borrowed navigation mesh.
/// @return Finite positive vertical tolerance in world units.
static float navmesh3d_vertical_tolerance(const rt_navmesh3d *nm) {
    double h = nm ? nm->agent_height : 0.0;
    if (!isfinite(h) || h <= 0.0)
        h = 0.0;
    return (float)fmax(h * 0.5, 0.25);
}

/// @brief Closest point to (`px`,`pz`) on segment a–b, projected onto the XZ plane.
/// @details Y is ignored (navmesh queries are horizontal). The parametric `t` is
///          clamped to `[0,1]` to stay on the segment; near-zero-length segments
///          collapse to endpoint `a`. Outputs the point and its squared XZ distance.
/// @param px Query X coordinate.
/// @param pz Query Z coordinate.
/// @param a Borrowed first segment endpoint.
/// @param b Borrowed second segment endpoint.
/// @param out_x Receives the closest X coordinate.
/// @param out_z Receives the closest Z coordinate.
/// @param out_d2 Receives squared XZ distance, or @c FLT_MAX for unusable input.
static void closest_point_on_segment_xz(
    float px, float pz, const float *a, const float *b, float *out_x, float *out_z, float *out_d2) {
    if (!isfinite(px) || !isfinite(pz) || !navmesh3d_float3_finite(a) ||
        !navmesh3d_float3_finite(b)) {
        *out_x = 0.0f;
        *out_z = 0.0f;
        *out_d2 = FLT_MAX;
        return;
    }
    float abx = b[0] - a[0];
    float abz = b[2] - a[2];
    float len_sq = abx * abx + abz * abz;
    float t = 0.0f;
    if (isfinite(len_sq) && len_sq > 1e-8f) {
        t = ((px - a[0]) * abx + (pz - a[2]) * abz) / len_sq;
        if (t < 0.0f)
            t = 0.0f;
        else if (t > 1.0f)
            t = 1.0f;
    }
    float cx = a[0] + abx * t;
    float cz = a[2] + abz * t;
    float dx = px - cx;
    float dz = pz - cz;
    *out_x = cx;
    *out_z = cz;
    *out_d2 = isfinite(dx * dx + dz * dz) ? dx * dx + dz * dz : FLT_MAX;
}

/// @brief Closest point to (`px`,`pz`) on triangle v0/v1/v2, projected onto XZ.
/// @details Returns the query point unchanged when it lies inside the triangle;
///          otherwise returns the nearest of the three edge projections.
/// @param px Query X coordinate.
/// @param pz Query Z coordinate.
/// @param v0 Borrowed first triangle vertex.
/// @param v1 Borrowed second triangle vertex.
/// @param v2 Borrowed third triangle vertex.
/// @param out_x Receives the closest X coordinate.
/// @param out_z Receives the closest Z coordinate.
static void closest_point_on_tri_xz(float px,
                                    float pz,
                                    const float *v0,
                                    const float *v1,
                                    const float *v2,
                                    float *out_x,
                                    float *out_z) {
    float best_x;
    float best_z;
    float best_d2;
    float cx;
    float cz;
    float d2;
    if (point_in_tri_xz(px, pz, v0, v1, v2)) {
        *out_x = px;
        *out_z = pz;
        return;
    }
    closest_point_on_segment_xz(px, pz, v0, v1, &best_x, &best_z, &best_d2);
    closest_point_on_segment_xz(px, pz, v1, v2, &cx, &cz, &d2);
    if (d2 < best_d2) {
        best_x = cx;
        best_z = cz;
        best_d2 = d2;
    }
    closest_point_on_segment_xz(px, pz, v2, v0, &cx, &cz, &d2);
    if (d2 < best_d2) {
        best_x = cx;
        best_z = cz;
    }
    *out_x = best_x;
    *out_z = best_z;
}

/// @brief Test one candidate triangle for containment of (px,pz); update the best (min |dy|) match.
/// @param nm Borrowed navmesh supplying live triangles and vertices.
/// @param i Candidate triangle index.
/// @param px Query X coordinate.
/// @param py Query Y coordinate.
/// @param pz Query Z coordinate.
/// @param max_dy Maximum allowed vertical separation, or a negative value to ignore it.
/// @param best_dy In/out smallest vertical separation observed.
/// @param best In/out index of the best containing triangle.
static void navmesh3d_find_tri_test(const rt_navmesh3d *nm,
                                    int32_t i,
                                    float px,
                                    float py,
                                    float pz,
                                    float max_dy,
                                    float *best_dy,
                                    int32_t *best) {
    const nav_triangle_t *t = &nm->triangles[i];
    if (t->blocked)
        return; /* carved out by an obstacle — not a valid point-location target */
    const float *v0 = nm->vertices[t->v[0]].position;
    const float *v1 = nm->vertices[t->v[1]].position;
    const float *v2 = nm->vertices[t->v[2]].position;
    if (point_in_tri_xz(px, pz, v0, v1, v2)) {
        float surface_y = triangle_y_at_xz(px, pz, v0, v1, v2);
        float dy = fabsf(py - surface_y);
        if (max_dy >= 0.0f && dy > max_dy)
            return;
        if (dy < *best_dy) {
            *best_dy = dy;
            *best = i;
        }
    }
}

/// @brief Find the walkable triangle under a point (XZ projection), nearest within @p max_dy in Y.
/// @details Uses the spatial query grid when built — a point lies in its own cell's triangle list,
///          so scanning one cell is exact — else falls back to a linear scan. @p max_dy < 0 ignores
///          vertical distance. Returns the triangle index, or -1 if none.
/// @param nm Borrowed navmesh supplying topology and the optional query grid.
/// @param px Query X coordinate.
/// @param py Query Y coordinate.
/// @param pz Query Z coordinate.
/// @param max_dy Maximum vertical separation, or a negative value to disable the limit.
/// @return Best containing walkable triangle index, or -1.
static int32_t find_tri_with_max_dy(
    const rt_navmesh3d *nm, float px, float py, float pz, float max_dy) {
    float best_dy = FLT_MAX;
    int32_t best = -1;
    if (!nm || !isfinite(px) || !isfinite(py) || !isfinite(pz) || !nm->triangles || !nm->vertices)
        return -1;
    /* Spatial-grid fast path: a point inside a triangle lies inside that triangle's XZ AABB, so the
     * triangle is registered in the point's cell — scanning that one cell is exact and matches the
     * linear scan. Points outside the grid bounds cannot lie on any triangle. */
    if (nm->qgrid_nx > 0 && nm->qgrid_starts && nm->qgrid_tris && isfinite(nm->qgrid_min_x) &&
        isfinite(nm->qgrid_min_z) && isfinite(nm->qgrid_inv_cell) && nm->qgrid_inv_cell > 0.0) {
        int32_t cx;
        int32_t cz;
        if (!navmesh3d_grid_axis_checked(
                (double)px, nm->qgrid_min_x, nm->qgrid_inv_cell, nm->qgrid_nx, &cx) ||
            !navmesh3d_grid_axis_checked(
                (double)pz, nm->qgrid_min_z, nm->qgrid_inv_cell, nm->qgrid_nz, &cz))
            return -1;
        size_t cell = (size_t)cz * (size_t)nm->qgrid_nx + (size_t)cx;
        for (int32_t k = nm->qgrid_starts[cell]; k < nm->qgrid_starts[cell + 1]; k++)
            navmesh3d_find_tri_test(nm, nm->qgrid_tris[k], px, py, pz, max_dy, &best_dy, &best);
        return best;
    }
    for (int32_t i = 0; i < nm->triangle_count; i++)
        navmesh3d_find_tri_test(nm, i, px, py, pz, max_dy, &best_dy, &best);
    return best;
}

/// @brief Find triangle containing point (projected onto XZ), ignoring vertical separation.
/// @param nm Borrowed navmesh.
/// @param px Query X coordinate.
/// @param py Query Y coordinate used only for deterministic stacked-surface selection.
/// @param pz Query Z coordinate.
/// @return Best containing walkable triangle index, or -1.
static int32_t find_tri(const rt_navmesh3d *nm, float px, float py, float pz) {
    return find_tri_with_max_dy(nm, px, py, pz, -1.0f);
}

/// @brief Test hook: verify the spatial query grid returns the same triangle as a linear scan.
/// @details Samples an N×N lattice over the navmesh bounds and checks each point's grid lookup
///          matches the brute-force result, guarding the grid acceleration against drift.
/// @param obj Opaque NavMesh3D handle.
/// @return 1 if every sample agrees (or there is no grid), 0 on any mismatch.
int8_t rt_navmesh3d_check_query_grid_parity(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    float min_x, min_z, max_x, max_z, mid_y;
    const int N = 40;
    if (!nm)
        return 0;
    if (nm->triangle_count <= 0 || nm->qgrid_nx <= 0)
        return 1; /* no grid: nothing to diverge from the linear scan */
    min_x = min_z = FLT_MAX;
    max_x = max_z = -FLT_MAX;
    {
        float min_y = FLT_MAX, max_y = -FLT_MAX;
        for (int32_t i = 0; i < nm->vertex_count; i++) {
            const float *p = nm->vertices[i].position;
            if (p[0] < min_x)
                min_x = p[0];
            if (p[0] > max_x)
                max_x = p[0];
            if (p[1] < min_y)
                min_y = p[1];
            if (p[1] > max_y)
                max_y = p[1];
            if (p[2] < min_z)
                min_z = p[2];
            if (p[2] > max_z)
                max_z = p[2];
        }
        mid_y = 0.5f * (min_y + max_y);
    }
    /* Sample a lattice spanning the bounds (slightly padded so out-of-bounds points are covered).
     */
    for (int iz = 0; iz <= N; iz++) {
        for (int ix = 0; ix <= N; ix++) {
            float fx = (float)ix / (float)N;
            float fz = (float)iz / (float)N;
            float px = min_x + (max_x - min_x) * (fx * 1.1f - 0.05f);
            float pz = min_z + (max_z - min_z) * (fz * 1.1f - 0.05f);
            float best_dy = FLT_MAX;
            int32_t grid_hit = find_tri_with_max_dy(nm, px, mid_y, pz, -1.0f);
            int32_t lin_hit = -1;
            for (int32_t i = 0; i < nm->triangle_count; i++)
                navmesh3d_find_tri_test(nm, i, px, mid_y, pz, -1.0f, &best_dy, &lin_hit);
            if (grid_hit != lin_hit)
                return 0;
        }
    }
    return 1;
}

/// @brief Drop the cached per-triangle offmesh adjacency list.
/// @param nm Mutable navmesh whose owned CSR arrays are freed and cleared.
static void navmesh3d_clear_offmesh_adjacency(rt_navmesh3d *nm) {
    if (!nm)
        return;
    free(nm->offmesh_adjacency_starts);
    free(nm->offmesh_adjacency_edges);
    nm->offmesh_adjacency_starts = NULL;
    nm->offmesh_adjacency_edges = NULL;
    nm->offmesh_adjacency_edge_count = 0;
}

/// @brief Pack an offmesh link index and direction bit for storage in the adjacency cache.
/// @details Returns -1 when the link index cannot fit into the signed packed representation.
/// @param link_index Non-negative authored link index.
/// @param reverse Non-zero to encode traversal from the link's destination back to its source.
/// @return Packed directed edge reference, or -1 when @p link_index is unrepresentable.
static nav_offmesh_edge_ref_t navmesh3d_pack_offmesh_edge(int32_t link_index, int reverse) {
    if (link_index < 0 || link_index > INT32_MAX / 2)
        return -1;
    return (nav_offmesh_edge_ref_t)((link_index << 1) | (reverse ? 1 : 0));
}

/// @brief Decode a packed offmesh adjacency reference into link index and reverse flag.
/// @param edge Packed directed edge reference.
/// @param out_link_index Optional output receiving the authored link index.
/// @param out_reverse Optional output receiving the direction bit.
/// @return 1 on success, 0 for malformed packed refs.
static int navmesh3d_unpack_offmesh_edge(nav_offmesh_edge_ref_t edge,
                                         int32_t *out_link_index,
                                         int *out_reverse) {
    if (edge < 0)
        return 0;
    if (out_link_index)
        *out_link_index = edge >> 1;
    if (out_reverse)
        *out_reverse = edge & 1;
    return 1;
}

/// @brief Rebuild the CSR offmesh-link adjacency cache from resolved link endpoint triangles.
/// @details A* expands links from the current triangle. Without this cache it must scan every
///   authored offmesh link for every expanded node. The cache stores only directed links incident
///   on each triangle. Allocation failure clears the cache and leaves pathfinding to use the
///   correct full-scan fallback.
/// @param nm Mutable navmesh whose derived CSR cache is replaced.
/// @return 1 when the cache is available or not needed, 0 when allocation failed.
static int navmesh3d_build_offmesh_adjacency(rt_navmesh3d *nm) {
    int32_t tri_count;
    int32_t link_count;
    int32_t directed_count = 0;
    int32_t *starts = NULL;
    int32_t *cursor = NULL;
    nav_offmesh_edge_ref_t *edges = NULL;

    navmesh3d_clear_offmesh_adjacency(nm);
    if (!nm)
        return 1;
    tri_count = navmesh3d_read_triangle_count(nm);
    link_count = navmesh3d_read_offmesh_link_count(nm);
    if (link_count <= 0 || tri_count <= 0)
        return 1;
    if (link_count > INT32_MAX / 2)
        return 0;
    if ((size_t)tri_count + 1u > SIZE_MAX / sizeof(int32_t))
        return 0;

    starts = (int32_t *)calloc((size_t)tri_count + 1u, sizeof(*starts));
    if (!starts)
        return 0;
    for (int32_t i = 0; i < link_count; i++) {
        const nav_offmesh_link_t *link = &nm->offmesh_links[i];
        if (link->from_tri >= 0 && link->from_tri < tri_count && link->to_tri >= 0 &&
            link->to_tri < tri_count) {
            starts[link->from_tri + 1]++;
            directed_count++;
        }
        if (link->bidirectional && link->to_tri >= 0 && link->to_tri < tri_count &&
            link->from_tri >= 0 && link->from_tri < tri_count) {
            starts[link->to_tri + 1]++;
            directed_count++;
        }
    }
    if (directed_count <= 0) {
        free(starts);
        return 1;
    }
    for (int32_t i = 0; i < tri_count; i++)
        starts[i + 1] += starts[i];
    if ((size_t)directed_count > SIZE_MAX / sizeof(*edges) ||
        (size_t)tri_count > SIZE_MAX / sizeof(*cursor)) {
        free(starts);
        return 0;
    }
    edges = (nav_offmesh_edge_ref_t *)malloc((size_t)directed_count * sizeof(*edges));
    cursor = (int32_t *)malloc((size_t)tri_count * sizeof(*cursor));
    if (!edges || !cursor) {
        free(starts);
        free(edges);
        free(cursor);
        return 0;
    }
    memcpy(cursor, starts, (size_t)tri_count * sizeof(*cursor));
    for (int32_t i = 0; i < link_count; i++) {
        const nav_offmesh_link_t *link = &nm->offmesh_links[i];
        nav_offmesh_edge_ref_t packed;
        if (link->from_tri >= 0 && link->from_tri < tri_count && link->to_tri >= 0 &&
            link->to_tri < tri_count) {
            packed = navmesh3d_pack_offmesh_edge(i, 0);
            if (packed >= 0)
                edges[cursor[link->from_tri]++] = packed;
        }
        if (link->bidirectional && link->to_tri >= 0 && link->to_tri < tri_count &&
            link->from_tri >= 0 && link->from_tri < tri_count) {
            packed = navmesh3d_pack_offmesh_edge(i, 1);
            if (packed >= 0)
                edges[cursor[link->to_tri]++] = packed;
        }
    }
    free(cursor);
    nm->offmesh_adjacency_starts = starts;
    nm->offmesh_adjacency_edges = edges;
    nm->offmesh_adjacency_edge_count = directed_count;
    return 1;
}

/// @brief Re-resolve authored off-mesh link endpoints against the current walkable triangle set.
/// @param nm Mutable navmesh whose link endpoint indices and adjacency cache are refreshed.
static void navmesh3d_refresh_offmesh_links(rt_navmesh3d *nm) {
    int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
    if (!nm)
        return;
    if (link_count <= 0) {
        navmesh3d_clear_offmesh_adjacency(nm);
        return;
    }
    float max_dy = navmesh3d_vertical_tolerance(nm);
    for (int32_t i = 0; i < link_count; i++) {
        nav_offmesh_link_t *link = &nm->offmesh_links[i];
        link->from_tri =
            find_tri_with_max_dy(nm, link->from[0], link->from[1], link->from[2], max_dy);
        link->to_tri = find_tri_with_max_dy(nm, link->to[0], link->to[1], link->to[2], max_dy);
    }
    (void)navmesh3d_build_offmesh_adjacency(nm);
}

/// @brief Validate and return one triangle's range in the derived off-mesh CSR cache.
/// @param nm Borrowed navmesh containing the derived cache.
/// @param triangle Triangle whose outgoing range is requested.
/// @param triangle_count Defensively bounded triangle count.
/// @param link_count Defensively bounded authored-link count.
/// @param out_begin Receives the inclusive edge offset.
/// @param out_end Receives the exclusive edge offset.
/// @return Non-zero only when the cache and requested range are internally consistent.
static int navmesh3d_offmesh_adjacency_range(const rt_navmesh3d *nm,
                                             int32_t triangle,
                                             int32_t triangle_count,
                                             int32_t link_count,
                                             int32_t *out_begin,
                                             int32_t *out_end) {
    if (!nm || !out_begin || !out_end || triangle < 0 || triangle >= triangle_count ||
        link_count <= 0 || !nm->offmesh_adjacency_starts || !nm->offmesh_adjacency_edges ||
        nm->offmesh_adjacency_edge_count <= 0 ||
        (int64_t)nm->offmesh_adjacency_edge_count > (int64_t)link_count * 2)
        return 0;
    int32_t begin = nm->offmesh_adjacency_starts[triangle];
    int32_t end = nm->offmesh_adjacency_starts[triangle + 1];
    if (begin < 0 || end < begin || end > nm->offmesh_adjacency_edge_count)
        return 0;
    *out_begin = begin;
    *out_end = end;
    return 1;
}

/// @brief Return the authored link connecting two triangles, if any. Sets @p reverse for to->from.
/// @param nm Borrowed navmesh containing authored links and optional adjacency cache.
/// @param from_tri Source triangle index.
/// @param to_tri Destination triangle index.
/// @param reverse Optional output set to one when a bidirectional link is traversed in reverse.
/// @return Borrowed matching link descriptor, or null when no permitted link connects the pair.
static const nav_offmesh_link_t *navmesh3d_find_offmesh_link(const rt_navmesh3d *nm,
                                                             int32_t from_tri,
                                                             int32_t to_tri,
                                                             int *reverse) {
    int32_t triangle_count = navmesh3d_read_triangle_count(nm);
    int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
    int32_t begin = 0;
    int32_t end = 0;
    if (reverse)
        *reverse = 0;
    if (!nm || link_count <= 0)
        return NULL;
    if (navmesh3d_offmesh_adjacency_range(nm, from_tri, triangle_count, link_count, &begin, &end)) {
        for (int32_t ei = begin; ei < end; ei++) {
            int32_t link_index = -1;
            int edge_reverse = 0;
            if (!navmesh3d_unpack_offmesh_edge(
                    nm->offmesh_adjacency_edges[ei], &link_index, &edge_reverse))
                continue;
            if (link_index < 0 || link_index >= link_count)
                continue;
            const nav_offmesh_link_t *link = &nm->offmesh_links[link_index];
            int32_t edge_to = edge_reverse ? link->from_tri : link->to_tri;
            if (edge_to == to_tri) {
                if (reverse)
                    *reverse = edge_reverse;
                return link;
            }
        }
        return NULL;
    }
    for (int32_t i = 0; i < link_count; i++) {
        const nav_offmesh_link_t *link = &nm->offmesh_links[i];
        if (link->from_tri == from_tri && link->to_tri == to_tri)
            return link;
        if (link->bidirectional && link->to_tri == from_tri && link->from_tri == to_tri) {
            if (reverse)
                *reverse = 1;
            return link;
        }
    }
    return NULL;
}
