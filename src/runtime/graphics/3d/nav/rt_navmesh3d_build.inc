//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/nav/rt_navmesh3d_build.inc
// Purpose: NavMesh3D construction, transactional adjacency/query indices,
//   tile rebuilding, areas, off-mesh links, and obstacles. This implementation
//   fragment is included by rt_navmesh3d.c and is not a standalone translation
//   unit.
//
// Key invariants:
//   - Derived topology and query grids are staged before replacing live state.
//   - Every allocation and replicated-cell product is checked before use.
//   - A first build may publish a correct linear-scan fallback; a failed
//     rebuild retains the prior published topology and query index.
//
// Ownership/Lifetime:
//   - Staging helpers own their temporary arrays until commit or discard.
//   - Committing a query-grid stage transfers its arrays to the NavMesh3D.
//
// Links: rt_navmesh3d.c, rt_navmesh3d_bake.inc, rt_navmesh3d_query.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_navmesh3d_build.inc
 * @brief Implements NavMesh3D construction, serialization, transactional topology publication,
 *        tile mutation, and scene-geometry collection.
 *
 * This private fragment is included by rt_navmesh3d.c. It validates all counts and arithmetic
 * before allocation, stages adjacency and spatial-grid representations before replacing live
 * state, and preserves the last published topology when a rebuild cannot be completed.
 */

/// @brief Clamp a logical count to the valid initialized range of an optional array.
/// @param array Borrowed array pointer.
/// @param count Requested logical element count.
/// @param capacity Allocated element capacity.
/// @return Zero for absent or invalid storage, otherwise `min(count, capacity)`.
static int32_t navmesh3d_clamped_array_count(const void *array, int32_t count, int32_t capacity) {
    if (!array || count <= 0 || capacity <= 0)
        return 0;
    return count < capacity ? count : capacity;
}

/// @brief Validate the bookkeeping for a mutable optional array.
/// @param array Borrowed array pointer.
/// @param count Number of initialized elements.
/// @param capacity Number of allocated elements.
/// @return Non-zero when appending or editing through the bookkeeping is safe.
static int navmesh3d_array_storage_valid(const void *array, int32_t count, int32_t capacity) {
    return count >= 0 && capacity >= 0 && count <= capacity && (capacity == 0 || array != NULL);
}

/// @brief Read a defensively bounded live-triangle count.
/// @param nm Borrowed navigation mesh.
/// @return Non-negative count constrained by available live and source triangle storage.
static int32_t navmesh3d_read_triangle_count(const rt_navmesh3d *nm) {
    if (!nm || !nm->triangles || nm->triangle_count <= 0)
        return 0;
    if (nm->source_triangles && nm->source_triangle_count > 0 &&
        nm->triangle_count > nm->source_triangle_count)
        return nm->source_triangle_count;
    return nm->triangle_count;
}

/// @brief Read a defensively bounded authored off-mesh-link count.
/// @param nm Borrowed navigation mesh.
/// @return Non-negative count constrained by the owned link array capacity.
static int32_t navmesh3d_read_offmesh_link_count(const rt_navmesh3d *nm) {
    return nm ? navmesh3d_clamped_array_count(
                    nm->offmesh_links, nm->offmesh_link_count, nm->offmesh_link_capacity)
              : 0;
}

/// @brief Read a defensively bounded authored obstacle count.
/// @param nm Borrowed navigation mesh.
/// @return Non-negative count constrained by the owned obstacle array capacity.
static int32_t navmesh3d_read_obstacle_count(const rt_navmesh3d *nm) {
    return nm ? navmesh3d_clamped_array_count(
                    nm->obstacles, nm->obstacle_count, nm->obstacle_capacity)
              : 0;
}

/// @brief Read a defensively bounded authored area-name count.
/// @param nm Borrowed navigation mesh.
/// @return Non-negative count constrained by the owned string array capacity.
static int32_t navmesh3d_read_area_name_count(const rt_navmesh3d *nm) {
    return nm ? navmesh3d_clamped_array_count(
                    nm->area_names, nm->area_name_count, nm->area_name_capacity)
              : 0;
}

static void navmesh3d_refresh_offmesh_links(rt_navmesh3d *nm);
static int point_in_tri_xz(float px, float pz, const float *v0, const float *v1, const float *v2);
static void navmesh3d_voxel_emit_tri(
    rt_navmesh3d *nm, int32_t a, int32_t b, int32_t c, int8_t blocked);
static void *navmesh3d_voxel_bake(
    rt_mesh3d *src, double agent_radius, double agent_height, double max_slope, double cell_size);

/// @brief GC finalizer for NavMesh3D. Frees the baked vertex and triangle arrays
/// (heap-allocated during `Build`) and nulls the slots so a stale handle would crash
/// loudly on use rather than reading freed memory. The source mesh is borrowed only
/// during the build call, not retained, so there's no reference to release here.
/// @param obj Owned NavMesh3D instance being finalized.
static void navmesh3d_finalizer(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)obj;
    if (nm->offmesh_links) {
        int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
        for (int32_t i = 0; i < link_count; i++) {
            if (nm->offmesh_links[i].kind && rt_string_is_handle(nm->offmesh_links[i].kind))
                rt_string_unref(nm->offmesh_links[i].kind);
        }
    }
    if (nm->area_names) {
        int32_t area_name_count = navmesh3d_read_area_name_count(nm);
        for (int32_t i = 0; i < area_name_count; i++) {
            if (nm->area_names[i] && rt_string_is_handle(nm->area_names[i]))
                rt_string_unref(nm->area_names[i]);
        }
    }
    free(nm->vertices);
    free(nm->source_triangles);
    free(nm->triangles);
    free(nm->offmesh_links);
    free(nm->offmesh_adjacency_starts);
    free(nm->offmesh_adjacency_edges);
    free(nm->area_names);
    free(nm->obstacles);
    free(nm->qgrid_starts);
    free(nm->qgrid_tris);
    for (int i = 0; i < NAVMESH3D_PATH_WORKSPACE_COUNT; i++) {
        free(nm->path_workspaces[i].g_cost);
        free(nm->path_workspaces[i].parent);
        free(nm->path_workspaces[i].closed);
        free(nm->path_workspaces[i].heap);
        free(nm->path_workspaces[i].corridor);
        free(nm->path_workspaces[i].portals);
    }
    if (nm->path_workspace_gate && rt_obj_release_check0(nm->path_workspace_gate))
        rt_obj_free(nm->path_workspace_gate);
    free(nm->voxel_cell_height);
    free(nm->voxel_cell_walkable);
    free(nm->voxel_corner_vertices);
    nm->vertices = NULL;
    nm->source_triangles = NULL;
    nm->triangles = NULL;
    nm->offmesh_links = NULL;
    nm->offmesh_adjacency_starts = NULL;
    nm->offmesh_adjacency_edges = NULL;
    nm->offmesh_adjacency_edge_count = 0;
    nm->area_names = NULL;
    nm->obstacles = NULL;
    nm->qgrid_starts = NULL;
    nm->qgrid_tris = NULL;
    memset(nm->path_workspaces, 0, sizeof(nm->path_workspaces));
    nm->path_workspace_gate = NULL;
    nm->path_active_queries = 0;
    nm->path_peak_active_queries = 0;
    nm->voxel_cell_height = NULL;
    nm->voxel_cell_walkable = NULL;
    nm->voxel_corner_vertices = NULL;
}

/// @brief Release a partially-constructed navmesh on allocation failure, freeing if refcount hits
/// zero.
/// @details Called in error paths where the navmesh object was allocated but bake failed,
///          so the half-built object must be dropped without going through the normal finalizer.
/// @param nm Partially initialized GC-managed navmesh whose local reference is released.
static void navmesh3d_free_partial(rt_navmesh3d *nm) {
    if (nm && rt_obj_release_check0(nm))
        rt_obj_free(nm);
}

/// @brief Drop one reference and free if zero. Safe on NULL.
/// @param obj Locally retained runtime object; null is a no-op.
static void navmesh3d_release_local(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Validate a max-slope angle (in degrees), clamping to `[0, 89.9]` on bad input.
/// @details Non-finite inputs collapse to 45° (the documented default).
/// @param degrees Requested maximum slope.
/// @return Finite slope in the inclusive supported range.
static double navmesh3d_sanitize_slope(double degrees) {
    if (!isfinite(degrees))
        degrees = 45.0;
    if (degrees < 0.0)
        degrees = 0.0;
    if (degrees > 89.9)
        degrees = 89.9;
    return degrees;
}

/// @brief Clamp a finite world coordinate to the range NavMesh3D can index robustly.
/// @param value Preferred coordinate.
/// @param fallback Replacement for non-finite input.
/// @return Finite coordinate in `[-NAVMESH3D_COORD_ABS_MAX, NAVMESH3D_COORD_ABS_MAX]`.
static double navmesh3d_coord_or(double value, double fallback) {
    if (!isfinite(fallback))
        fallback = 0.0;
    if (!isfinite(value))
        value = fallback;
    if (value > NAVMESH3D_COORD_ABS_MAX)
        return NAVMESH3D_COORD_ABS_MAX;
    if (value < -NAVMESH3D_COORD_ABS_MAX)
        return -NAVMESH3D_COORD_ABS_MAX;
    return value;
}

/// @brief Clamp a coordinate to a finite float after applying the NavMesh3D coordinate range.
/// @param value Preferred coordinate.
/// @param fallback Replacement for non-finite input.
/// @return Sanitized coordinate converted to float.
static float navmesh3d_coordf_or(double value, double fallback) {
    return (float)navmesh3d_coord_or(value, fallback);
}

/// @brief Sanitize positive agent dimensions such as radius/height.
/// @param value Preferred dimension.
/// @param fallback Positive default for unusable input.
/// @return Positive finite dimension capped to @c NAVMESH3D_AGENT_DIM_MAX.
static double navmesh3d_agent_dim_or(double value, double fallback) {
    value = (isfinite(value) && value > 0.0) ? value : fallback;
    if (!isfinite(value) || value <= 0.0)
        value = fallback;
    if (value > NAVMESH3D_AGENT_DIM_MAX)
        value = NAVMESH3D_AGENT_DIM_MAX;
    return value;
}

/// @brief Sanitize voxel cell size to a finite, bounded positive value.
/// @param value Preferred cell size.
/// @param fallback Positive default for unusable input.
/// @return Finite cell size within the configured minimum and maximum.
static double navmesh3d_cell_size_or(double value, double fallback) {
    value = (isfinite(value) && value > 0.0) ? value : fallback;
    if (!isfinite(value) || value <= 0.0)
        value = fallback;
    if (value < NAVMESH3D_CELL_SIZE_MIN)
        value = NAVMESH3D_CELL_SIZE_MIN;
    if (value > NAVMESH3D_CELL_SIZE_MAX)
        value = NAVMESH3D_CELL_SIZE_MAX;
    return value;
}

/// @brief Read a Vec3 point into finite float coordinates for NavMesh3D queries.
/// @param point Borrowed runtime Vec3 handle.
/// @param out Caller-owned three-float destination.
/// @return 1 when the handle and all coordinates are valid, otherwise 0.
static int navmesh3d_read_point3f(void *point, float out[3]) {
    if (!rt_g3d_is_vec3(point) || !out)
        return 0;
    double x = rt_vec3_x(point);
    double y = rt_vec3_y(point);
    double z = rt_vec3_z(point);
    if (!isfinite(x) || !isfinite(y) || !isfinite(z))
        return 0;
    out[0] = navmesh3d_coordf_or(x, 0.0);
    out[1] = navmesh3d_coordf_or(y, 0.0);
    out[2] = navmesh3d_coordf_or(z, 0.0);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

/// @brief True when all lanes of a float3 are finite.
/// @param p Borrowed three-float vector.
/// @return Non-zero when @p p is non-null and all components are finite.
static int navmesh3d_float3_finite(const float *p) {
    return p && isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]);
}

/// @brief Reject tile indices that would overflow tile-to-world coordinate math.
/// @param tile Signed tile coordinate.
/// @return Non-zero when @p tile lies within the configured absolute bound.
static int navmesh3d_tile_index_valid(int64_t tile) {
    return tile >= -NAVMESH3D_TILE_INDEX_ABS_MAX && tile <= NAVMESH3D_TILE_INDEX_ABS_MAX;
}

/// @brief Whether the shared edge is wide enough for the configured agent radius.
/// @param nm Borrowed navmesh containing vertices and agent-clearance policy.
/// @param va First shared-edge vertex index.
/// @param vb Second shared-edge vertex index.
/// @return 1 when the portal is valid and sufficiently wide, otherwise 0.
static int navmesh3d_portal_allows_agent(const rt_navmesh3d *nm, int32_t va, int32_t vb) {
    if (!nm || !nm->vertices || va < 0 || vb < 0 || va >= nm->vertex_count ||
        vb >= nm->vertex_count)
        return 0;
    /* Voxel-baked navmeshes already eroded the walkable set by the agent radius, so every surviving
     * grid edge is agent-safe; gating those (cell_size-wide) portals by 2*radius would fragment the
     * mesh. The width gate only applies to the Build() passthrough path over authored triangles. */
    if (nm->skip_portal_width_gate)
        return 1;
    if (!isfinite(nm->agent_radius) || nm->agent_radius <= 0.0)
        return 1;
    const float *a = nm->vertices[va].position;
    const float *b = nm->vertices[vb].position;
    double dx = (double)b[0] - (double)a[0];
    double dz = (double)b[2] - (double)a[2];
    double width = hypot(dx, dz);
    return width + 1e-5 >= nm->agent_radius * 2.0 ? 1 : 0;
}

/// @brief Clamp a traversal cost to [1.0, 1e6]; non-finite or sub-1.0 values become 1.0.
/// @param cost Requested traversal-cost multiplier.
/// @return Finite float multiplier in `[1, 1e6]`.
static float navmesh3d_sanitize_traversal_cost(double cost) {
    if (!isfinite(cost) || cost < 1.0)
        cost = 1.0;
    if (cost > 1000000.0)
        cost = 1000000.0;
    return (float)cost;
}

/// @brief Write @p bytes raw bytes to @p f; returns nonzero on success (or when nothing to write).
/// @param f Open destination stream.
/// @param data Borrowed source buffer.
/// @param bytes Number of bytes to write.
/// @return Non-zero when the complete request succeeds.
static int navmesh3d_io_write(FILE *f, const void *data, size_t bytes) {
    return f && (!bytes || fwrite(data, 1u, bytes, f) == bytes);
}

/// @brief Read @p bytes raw bytes from @p f; returns nonzero on success (or when nothing to read).
/// @param f Open source stream.
/// @param data Caller-owned destination buffer.
/// @param bytes Number of bytes to read.
/// @return Non-zero when the complete request succeeds.
static int navmesh3d_io_read(FILE *f, void *data, size_t bytes) {
    return f && (!bytes || fread(data, 1u, bytes, f) == bytes);
}

/// @brief Write a single uint8 to @p f.
/// @param f Open destination stream.
/// @param v Value to serialize.
/// @return Non-zero on success.
static int navmesh3d_write_u8(FILE *f, uint8_t v) {
    return navmesh3d_io_write(f, &v, 1u);
}

/// @brief Read a single uint8 from @p f into @p out.
/// @param f Open source stream.
/// @param out Receives the decoded value.
/// @return Non-zero on success.
static int navmesh3d_read_u8(FILE *f, uint8_t *out) {
    return out && navmesh3d_io_read(f, out, 1u);
}

/// @brief Write a uint32 to @p f in little-endian byte order.
/// @param f Open destination stream.
/// @param v Value to serialize.
/// @return Non-zero on success.
static int navmesh3d_write_u32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v & 0xffu),
                    (uint8_t)((v >> 8) & 0xffu),
                    (uint8_t)((v >> 16) & 0xffu),
                    (uint8_t)((v >> 24) & 0xffu)};
    return navmesh3d_io_write(f, b, sizeof(b));
}

/// @brief Read a little-endian uint32 from @p f into @p out.
/// @param f Open source stream.
/// @param out Receives the decoded value.
/// @return Non-zero on success.
static int navmesh3d_read_u32(FILE *f, uint32_t *out) {
    uint8_t b[4];
    if (!out || !navmesh3d_io_read(f, b, sizeof(b)))
        return 0;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}

/// @brief Write an int32 to @p f (little-endian two's-complement).
/// @param f Open destination stream.
/// @param v Value to serialize.
/// @return Non-zero on success.
static int navmesh3d_write_i32(FILE *f, int32_t v) {
    return navmesh3d_write_u32(f, (uint32_t)v);
}

/// @brief Read a little-endian int32 from @p f into @p out.
/// @param f Open source stream.
/// @param out Receives the decoded value.
/// @return Non-zero on success.
static int navmesh3d_read_i32(FILE *f, int32_t *out) {
    uint32_t u;
    if (!out || !navmesh3d_read_u32(f, &u))
        return 0;
    *out = (int32_t)u;
    return 1;
}

/// @brief Write a uint64 to @p f in little-endian byte order.
/// @param f Open destination stream.
/// @param v Value to serialize.
/// @return Non-zero on success.
static int navmesh3d_write_u64(FILE *f, uint64_t v) {
    uint8_t b[8] = {(uint8_t)(v & 0xffu),
                    (uint8_t)((v >> 8) & 0xffu),
                    (uint8_t)((v >> 16) & 0xffu),
                    (uint8_t)((v >> 24) & 0xffu),
                    (uint8_t)((v >> 32) & 0xffu),
                    (uint8_t)((v >> 40) & 0xffu),
                    (uint8_t)((v >> 48) & 0xffu),
                    (uint8_t)((v >> 56) & 0xffu)};
    return navmesh3d_io_write(f, b, sizeof(b));
}

/// @brief Read a little-endian uint64 from @p f into @p out.
/// @param f Open source stream.
/// @param out Receives the decoded value.
/// @return Non-zero on success.
static int navmesh3d_read_u64(FILE *f, uint64_t *out) {
    uint8_t b[8];
    if (!out || !navmesh3d_io_read(f, b, sizeof(b)))
        return 0;
    *out = (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) |
           ((uint64_t)b[3] << 24) | ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40) |
           ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
    return 1;
}

/// @brief Write an int64 to @p f (little-endian two's-complement).
/// @param f Open destination stream.
/// @param v Value to serialize.
/// @return Non-zero on success.
static int navmesh3d_write_i64(FILE *f, int64_t v) {
    return navmesh3d_write_u64(f, (uint64_t)v);
}

/// @brief Read a little-endian int64 from @p f into @p out.
/// @param f Open source stream.
/// @param out Receives the decoded value.
/// @return Non-zero on success.
static int navmesh3d_read_i64(FILE *f, int64_t *out) {
    uint64_t u;
    if (!out || !navmesh3d_read_u64(f, &u))
        return 0;
    *out = (int64_t)u;
    return 1;
}

/// @brief Write a float to @p f as a little-endian uint32 bit pattern (non-finite stored as 0).
/// @param f Open destination stream.
/// @param v Float value to serialize.
/// @return Non-zero on success.
static int navmesh3d_write_f32(FILE *f, float v) {
    uint32_t u;
    if (!isfinite(v))
        v = 0.0f;
    memcpy(&u, &v, sizeof(u));
    return navmesh3d_write_u32(f, u);
}

/// @brief Read a little-endian float bit pattern from @p f, sanitizing non-finite/out-of-range to
/// 0.
/// @param f Open source stream.
/// @param out Receives the sanitized decoded float.
/// @return Non-zero on success.
static int navmesh3d_read_f32(FILE *f, float *out) {
    uint32_t u;
    if (!out || !navmesh3d_read_u32(f, &u))
        return 0;
    memcpy(out, &u, sizeof(*out));
    *out = navmesh3d_coordf_or((double)*out, 0.0);
    return 1;
}

/// @brief Write a double to @p f as a little-endian uint64 bit pattern (non-finite stored as 0).
/// @param f Open destination stream.
/// @param v Double value to serialize.
/// @return Non-zero on success.
static int navmesh3d_write_f64(FILE *f, double v) {
    uint64_t u;
    if (!isfinite(v))
        v = 0.0;
    memcpy(&u, &v, sizeof(u));
    return navmesh3d_write_u64(f, u);
}

/// @brief Read a little-endian double bit pattern from @p f into @p out.
/// @param f Open source stream.
/// @param out Receives the decoded double bit pattern.
/// @return Non-zero on success.
static int navmesh3d_read_f64(FILE *f, double *out) {
    uint64_t u;
    if (!out || !navmesh3d_read_u64(f, &u))
        return 0;
    memcpy(out, &u, sizeof(*out));
    return 1;
}

/// @brief Write a string to @p f as a uint32 length prefix followed by its raw UTF-8 bytes.
/// @param f Open destination stream.
/// @param s Optional runtime string; invalid input is serialized as empty.
/// @return Non-zero on success, otherwise zero for an oversized string or I/O failure.
static int navmesh3d_write_string(FILE *f, rt_string s) {
    const char *bytes = (s && rt_string_is_handle(s)) ? rt_string_cstr(s) : "";
    int64_t stored_len = s && rt_string_is_handle(s) ? rt_str_len(s) : 0;
    if (!bytes || stored_len < 0 || (uint64_t)stored_len > UINT32_MAX)
        return 0;
    size_t len = (size_t)stored_len;
    return navmesh3d_write_u32(f, (uint32_t)len) &&
           (len == 0u || navmesh3d_io_write(f, bytes, len));
}

/// @brief Read a length-prefixed string from @p f (rejecting lengths above @p max_len) into @p out.
/// @param f Open source stream.
/// @param out Receives a newly allocated runtime string on success.
/// @param max_len Maximum accepted byte length.
/// @return Non-zero on success, otherwise zero without a usable published string.
static int navmesh3d_read_string(FILE *f, rt_string *out, uint32_t max_len) {
    uint32_t len;
    char *buf;
    if (!out || !navmesh3d_read_u32(f, &len) || len > max_len)
        return 0;
    *out = NULL;
    if (len == 0) {
        *out = rt_string_from_bytes("", 0u);
        return *out != NULL;
    }
    buf = (char *)malloc((size_t)len);
    if (!buf)
        return 0;
    if (!navmesh3d_io_read(f, buf, (size_t)len)) {
        free(buf);
        return 0;
    }
    *out = rt_string_from_bytes(buf, (size_t)len);
    free(buf);
    return *out != NULL;
}

/// @brief Return true when point (x,z) lies inside an axis-aligned XZ rectangle.
/// @param x Query X coordinate.
/// @param z Query Z coordinate.
/// @param min Borrowed three-component rectangle minimum.
/// @param max Borrowed three-component rectangle maximum.
/// @return Non-zero when the query lies inside the inclusive epsilon-expanded rectangle.
static int navmesh3d_point_in_rect_xz(float x, float z, const float *min, const float *max) {
    const float eps = 1e-5f;
    return x >= min[0] - eps && x <= max[0] + eps && z >= min[2] - eps && z <= max[2] + eps;
}

/// @brief Signed orientation (twice the triangle area) of points A, B, C in the XZ plane;
///   the sign gives the turn direction and zero means collinear.
/// @param ax Point A X coordinate.
/// @param az Point A Z coordinate.
/// @param bx Point B X coordinate.
/// @param bz Point B Z coordinate.
/// @param cx Point C X coordinate.
/// @param cz Point C Z coordinate.
/// @return Signed doubled XZ triangle area.
static float navmesh3d_orient_xz(float ax, float az, float bx, float bz, float cx, float cz) {
    return (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
}

/// @brief True if point (px, pz) is collinear with and lies within segment A→B in the XZ
///   plane (with a small epsilon tolerance).
/// @param ax Segment start X.
/// @param az Segment start Z.
/// @param bx Segment end X.
/// @param bz Segment end Z.
/// @param px Query point X.
/// @param pz Query point Z.
/// @return Non-zero when the query lies on the closed segment within tolerance.
static int navmesh3d_on_segment_xz(float ax, float az, float bx, float bz, float px, float pz) {
    const float eps = 1e-5f;
    if (fabsf(navmesh3d_orient_xz(ax, az, bx, bz, px, pz)) > eps)
        return 0;
    return px >= fminf(ax, bx) - eps && px <= fmaxf(ax, bx) + eps && pz >= fminf(az, bz) - eps &&
           pz <= fmaxf(az, bz) + eps;
}

/// @brief Closed segment intersection in the XZ plane.
/// @param ax First segment start X.
/// @param az First segment start Z.
/// @param bx First segment end X.
/// @param bz First segment end Z.
/// @param cx Second segment start X.
/// @param cz Second segment start Z.
/// @param dx Second segment end X.
/// @param dz Second segment end Z.
/// @return Non-zero when the closed segments intersect within tolerance.
static int navmesh3d_segments_intersect_xz(
    float ax, float az, float bx, float bz, float cx, float cz, float dx, float dz) {
    float o1 = navmesh3d_orient_xz(ax, az, bx, bz, cx, cz);
    float o2 = navmesh3d_orient_xz(ax, az, bx, bz, dx, dz);
    float o3 = navmesh3d_orient_xz(cx, cz, dx, dz, ax, az);
    float o4 = navmesh3d_orient_xz(cx, cz, dx, dz, bx, bz);
    const float eps = 1e-5f;
    if (((o1 > eps && o2 < -eps) || (o1 < -eps && o2 > eps)) &&
        ((o3 > eps && o4 < -eps) || (o3 < -eps && o4 > eps)))
        return 1;
    if (fabsf(o1) <= eps && navmesh3d_on_segment_xz(ax, az, bx, bz, cx, cz))
        return 1;
    if (fabsf(o2) <= eps && navmesh3d_on_segment_xz(ax, az, bx, bz, dx, dz))
        return 1;
    if (fabsf(o3) <= eps && navmesh3d_on_segment_xz(cx, cz, dx, dz, ax, az))
        return 1;
    if (fabsf(o4) <= eps && navmesh3d_on_segment_xz(cx, cz, dx, dz, bx, bz))
        return 1;
    return 0;
}

/// @brief Whether a triangle's exact XZ footprint intersects an AABB footprint and vertical span.
/// @param nm Borrowed navmesh supplying triangle vertices.
/// @param tri Borrowed triangle descriptor.
/// @param min Borrowed AABB minimum corner.
/// @param max Borrowed AABB maximum corner.
/// @return Non-zero when the triangle and box overlap.
static int navmesh3d_triangle_overlaps_bounds(const rt_navmesh3d *nm,
                                              const nav_triangle_t *tri,
                                              const float *min,
                                              const float *max) {
    if (!nm || !tri || !min || !max || !nm->vertices)
        return 0;
    const float *p0 = nm->vertices[tri->v[0]].position;
    const float *p1 = nm->vertices[tri->v[1]].position;
    const float *p2 = nm->vertices[tri->v[2]].position;
    float tri_min[3] = {
        fminf(p0[0], fminf(p1[0], p2[0])),
        fminf(p0[1], fminf(p1[1], p2[1])),
        fminf(p0[2], fminf(p1[2], p2[2])),
    };
    float tri_max[3] = {
        fmaxf(p0[0], fmaxf(p1[0], p2[0])),
        fmaxf(p0[1], fmaxf(p1[1], p2[1])),
        fmaxf(p0[2], fmaxf(p1[2], p2[2])),
    };
    if (tri_max[0] < min[0] || tri_min[0] > max[0] || tri_max[1] < min[1] || tri_min[1] > max[1] ||
        tri_max[2] < min[2] || tri_min[2] > max[2])
        return 0;

    if (navmesh3d_point_in_rect_xz(p0[0], p0[2], min, max) ||
        navmesh3d_point_in_rect_xz(p1[0], p1[2], min, max) ||
        navmesh3d_point_in_rect_xz(p2[0], p2[2], min, max))
        return 1;

    float r0[2] = {min[0], min[2]};
    float r1[2] = {max[0], min[2]};
    float r2[2] = {max[0], max[2]};
    float r3[2] = {min[0], max[2]};
    if (point_in_tri_xz(r0[0], r0[1], p0, p1, p2) || point_in_tri_xz(r1[0], r1[1], p0, p1, p2) ||
        point_in_tri_xz(r2[0], r2[1], p0, p1, p2) || point_in_tri_xz(r3[0], r3[1], p0, p1, p2))
        return 1;

    const float *tv[3] = {p0, p1, p2};
    const float *rv[4] = {r0, r1, r2, r3};
    for (int e = 0; e < 3; e++) {
        const float *a = tv[e];
        const float *b = tv[(e + 1) % 3];
        for (int re = 0; re < 4; re++) {
            const float *c = rv[re];
            const float *d = rv[(re + 1) % 4];
            if (navmesh3d_segments_intersect_xz(a[0], a[2], b[0], b[2], c[0], c[1], d[0], d[1]))
                return 1;
        }
    }
    return 0;
}

/// @brief Whether a triangle overlaps any authored obstacle volume.
/// @param nm Borrowed navmesh containing obstacles and vertex data.
/// @param tri Borrowed triangle descriptor.
/// @return Non-zero when any authored obstacle overlaps the triangle.
static int navmesh3d_triangle_blocked_by_obstacle(const rt_navmesh3d *nm,
                                                  const nav_triangle_t *tri) {
    int32_t obstacle_count = navmesh3d_read_obstacle_count(nm);
    if (!nm || !tri || obstacle_count <= 0 || !nm->vertices)
        return 0;
    for (int32_t i = 0; i < obstacle_count; i++) {
        const nav_obstacle_t *ob = &nm->obstacles[i];
        if (!navmesh3d_triangle_overlaps_bounds(nm, tri, ob->min, ob->max))
            continue;
        return 1;
    }
    return 0;
}

/// @brief Intern a nav-area name, returning its 1-based area id and appending it (growing the
///   name table) if new.
/// @param nm Mutable navmesh owning the intern table.
/// @param area Borrowed runtime string to intern.
/// @return The 1-based id, 0 for an empty name, or -1 on invalid input or allocation failure.
static int32_t navmesh3d_ensure_area_id(rt_navmesh3d *nm, rt_string area) {
    int64_t name_len64;
    size_t name_len;
    if (!nm || !area || !rt_string_is_handle(area))
        return -1;
    if (nm->area_name_count < 0 || nm->area_name_capacity < 0 ||
        nm->area_name_count > nm->area_name_capacity ||
        (nm->area_name_capacity > 0 && !nm->area_names))
        return -1;
    const char *name = rt_string_cstr(area);
    name_len64 = rt_str_len(area);
    if (!name || name_len64 < 0)
        return -1;
    if (name_len64 == 0)
        return 0;
    name_len = (size_t)name_len64;
    for (int32_t i = 0; i < nm->area_name_count; i++) {
        rt_string existing_handle = nm->area_names[i];
        if (!existing_handle || !rt_string_is_handle(existing_handle))
            return -1;
        const char *existing = rt_string_cstr(existing_handle);
        int64_t existing_len64 = rt_str_len(existing_handle);
        if (!existing || existing_len64 < 0)
            return -1;
        if ((size_t)existing_len64 == name_len && memcmp(existing, name, name_len) == 0)
            return i + 1;
    }
    if (nm->area_name_count >= nm->area_name_capacity) {
        int32_t new_cap;
        if (nm->area_name_count == INT32_MAX)
            return -1;
        if (nm->area_name_capacity <= 0)
            new_cap = 4;
        else if (nm->area_name_capacity > INT32_MAX / 2)
            new_cap = nm->area_name_count + 1;
        else
            new_cap = nm->area_name_capacity * 2;
        if (new_cap <= nm->area_name_count || (size_t)new_cap > SIZE_MAX / sizeof(rt_string))
            return -1;
        rt_string *areas =
            (rt_string *)realloc(nm->area_names, (size_t)new_cap * sizeof(rt_string));
        if (!areas)
            return -1;
        for (int32_t i = nm->area_name_capacity; i < new_cap; i++)
            areas[i] = NULL;
        nm->area_names = areas;
        nm->area_name_capacity = new_cap;
    }
    rt_string retained = rt_string_ref(area);
    if (!retained)
        return -1;
    nm->area_names[nm->area_name_count] = retained;
    nm->area_name_count++;
    return nm->area_name_count;
}

/// @brief Reverse-lookup the name for a 1-based area id; returns "default" for id 0 or an
///   invalid/unset id.
/// @param nm Borrowed navmesh containing interned names.
/// @param area_id One-based area identifier.
/// @return Retained authored name, or the shared `default` string.
static rt_string navmesh3d_area_name(const rt_navmesh3d *nm, int32_t area_id) {
    int32_t area_name_count = navmesh3d_read_area_name_count(nm);
    if (area_id > 0 && area_id <= area_name_count && nm->area_names[area_id - 1] &&
        rt_string_is_handle(nm->area_names[area_id - 1]))
        return rt_string_ref(nm->area_names[area_id - 1]);
    return rt_const_cstr("default");
}

/// @brief Sanitized traversal cost of triangle @p tri (1.0 if @p tri is out of range).
/// @param nm Borrowed navmesh.
/// @param tri Triangle index.
/// @return Finite multiplier in `[1, 1e6]`.
static float navmesh3d_tri_cost(const rt_navmesh3d *nm, int32_t tri) {
    if (!nm || !nm->triangles || tri < 0 || tri >= navmesh3d_read_triangle_count(nm))
        return 1.0f;
    return navmesh3d_sanitize_traversal_cost((double)nm->triangles[tri].traversal_cost);
}

/// @brief Mix an undirected edge key into a well-distributed 32-bit hash value.
///
/// Edge keys store `(min_vertex << 32) | max_vertex`. Using only the low bits
/// clusters meshes whose high endpoint changes slowly across strips or grids.
/// This SplitMix64 finalizer folds both vertex ids before the adjacency table
/// takes its modulo, reducing probe chains on large baked navmeshes.
/// @param key Packed undirected edge key.
/// @return 32-bit mixed hash suitable for modulo table indexing.
static uint32_t navmesh3d_hash_edge_key(uint64_t key) {
    key += UINT64_C(0x9E3779B97F4A7C15);
    key = (key ^ (key >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    key = (key ^ (key >> 27)) * UINT64_C(0x94D049BB133111EB);
    key ^= key >> 31;
    return (uint32_t)(key ^ (key >> 32));
}

/// @brief Stage and atomically publish edge adjacency for an explicit triangle array.
/// @details A temporary neighbor array and edge hash are completed before any `neighbors` member in
///   @p triangles changes. Hash entries match `(min_vertex << 32) | max_vertex`; the first owner is
///   recorded and the second owner is cross-linked when the portal admits the configured agent. A
///   third owner is a non-manifold edge and rejects the complete rebuild. Boundary edges remain -1.
/// @param nm Navigation mesh supplying validated vertices and agent-width policy.
/// @param triangles Triangle array whose neighbor lanes are replaced only after full success.
/// @param tc Number of elements available in @p triangles.
/// @return 1 after committing a complete adjacency graph, or 0 on invalid input, non-manifold
///   topology, checked-size failure, injected failure, or allocation failure. On zero, every input
///   triangle retains its previous neighbor lanes.
static int navmesh3d_build_adjacency_for(rt_navmesh3d *nm, nav_triangle_t *triangles, int32_t tc) {
    int32_t map_cap;
    int32_t *neighbors = NULL;
    size_t neighbor_slots;
    if (!nm || tc < 0 || (tc > 0 && !triangles))
        return 0;
    if (tc <= 0)
        return 1;
    if (g_navmesh3d_test_force_adjacency_alloc_failure || tc > INT32_MAX / 4 ||
        (size_t)tc > SIZE_MAX / 3u)
        return 0;
    neighbor_slots = (size_t)tc * 3u;
    if (neighbor_slots > SIZE_MAX / sizeof(*neighbors))
        return 0;
    neighbors = (int32_t *)malloc(neighbor_slots * sizeof(*neighbors));
    if (!neighbors)
        return 0;
    for (size_t i = 0; i < neighbor_slots; i++)
        neighbors[i] = -1;

    map_cap = tc * 4;
    if (map_cap < 16)
        map_cap = 16;

    typedef struct {
        uint64_t key;
        int32_t tri_idx;
        int32_t edge_idx;
        int8_t used;
        int8_t matched;
    } edge_entry_t;

    if ((size_t)map_cap > SIZE_MAX / sizeof(edge_entry_t)) {
        free(neighbors);
        return 0;
    }
    edge_entry_t *emap = (edge_entry_t *)calloc((size_t)map_cap, sizeof(edge_entry_t));
    if (!emap) {
        free(neighbors);
        return 0;
    }

    for (int32_t i = 0; i < tc; i++) {
        for (int e = 0; e < 3; e++) {
            int32_t va = triangles[i].v[e];
            int32_t vb = triangles[i].v[(e + 1) % 3];
            int32_t lo = va < vb ? va : vb;
            int32_t hi = va < vb ? vb : va;
            int matched_or_inserted = 0;
            if (lo < 0 || hi < 0 || lo >= nm->vertex_count || hi >= nm->vertex_count)
                goto adjacency_fail;
            uint64_t key = ((uint64_t)(uint32_t)lo << 32) | (uint32_t)hi;
            uint32_t slot = navmesh3d_hash_edge_key(key) % (uint32_t)map_cap;
            for (int probe = 0; probe < map_cap; probe++) {
                uint32_t idx = (slot + (uint32_t)probe) % (uint32_t)map_cap;
                if (!emap[idx].used) {
                    emap[idx].key = key;
                    emap[idx].tri_idx = i;
                    emap[idx].edge_idx = e;
                    emap[idx].used = 1;
                    matched_or_inserted = 1;
                    break;
                }
                if (emap[idx].key == key) {
                    int32_t j = emap[idx].tri_idx;
                    int32_t je = emap[idx].edge_idx;
                    if (emap[idx].matched)
                        goto adjacency_fail;
                    emap[idx].matched = 1;
                    if (neighbors[(size_t)i * 3u + (size_t)e] >= 0 ||
                        neighbors[(size_t)j * 3u + (size_t)je] >= 0)
                        goto adjacency_fail;
                    if (navmesh3d_portal_allows_agent(nm, lo, hi)) {
                        neighbors[(size_t)i * 3u + (size_t)e] = j;
                        neighbors[(size_t)j * 3u + (size_t)je] = i;
                    }
                    matched_or_inserted = 1;
                    break;
                }
            }
            if (!matched_or_inserted)
                goto adjacency_fail;
        }
    }
    for (int32_t i = 0; i < tc; i++)
        memcpy(triangles[i].neighbors, &neighbors[(size_t)i * 3u], 3u * sizeof(int32_t));
    free(emap);
    free(neighbors);
    return 1;

adjacency_fail:
    free(emap);
    free(neighbors);
    return 0;
}

/// @brief Atomically rebuild adjacency for the current live triangle array.
/// @param nm Navigation mesh whose live triangles are to be rebuilt.
/// @return 1 on success, or 0 with all prior neighbor lanes retained.
static int navmesh3d_build_adjacency(rt_navmesh3d *nm) {
    if (!nm)
        return 0;
    return navmesh3d_build_adjacency_for(nm, nm->triangles, nm->triangle_count);
}

/// @brief Filter source triangles by max-slope angle, keeping only walkable surfaces.
/// @details Compares each triangle's surface-normal Y component against `cos(max_slope)`;
///          triangles with normals pitched too steeply are dropped. Rebuilds adjacency
///          afterwards via `navmesh3d_build_adjacency`.

/// @brief Release the spatial query-grid index and reset all derived grid metadata.
/// @param nm Navigation mesh that exclusively owns the live grid arrays.
static void navmesh3d_free_query_grid(rt_navmesh3d *nm) {
    if (!nm)
        return;
    free(nm->qgrid_starts);
    free(nm->qgrid_tris);
    nm->qgrid_starts = NULL;
    nm->qgrid_tris = NULL;
    nm->qgrid_min_x = 0.0;
    nm->qgrid_min_z = 0.0;
    nm->qgrid_inv_cell = 0.0;
    nm->qgrid_nx = 0;
    nm->qgrid_nz = 0;
}

/// @brief Complete temporary ownership for one candidate query-grid representation.
/// @details An all-zero value represents the correct non-grid linear-scan fallback. Non-NULL array
///   members are exclusively owned by the stage until committed or discarded.
typedef struct {
    double min_x;
    double min_z;
    double inv_cell;
    int32_t nx;
    int32_t nz;
    int32_t *starts;
    int32_t *tris;
} navmesh3d_query_grid_stage;

/// @brief Result category for query-grid staging.
typedef enum {
    NAVMESH3D_QUERY_GRID_ERROR = 0,
    NAVMESH3D_QUERY_GRID_READY = 1,
    NAVMESH3D_QUERY_GRID_BOUNDED_FALLBACK = 2,
} navmesh3d_query_grid_result;

/// @brief Release every array owned by a query-grid stage and reset it to the empty fallback.
/// @param stage Stage to discard; NULL is accepted.
static void navmesh3d_discard_query_grid_stage(navmesh3d_query_grid_stage *stage) {
    if (!stage)
        return;
    free(stage->starts);
    free(stage->tris);
    memset(stage, 0, sizeof(*stage));
}

/// @brief Replace a live spatial grid with one complete staged representation.
/// @details Old arrays are released only after staging has succeeded. Ownership of @p stage arrays
///   transfers to @p nm and the stage is reset so cleanup cannot double-free them.
/// @param nm Navigation mesh receiving the staged representation.
/// @param stage Complete grid or all-zero bounded fallback to publish.
static void navmesh3d_commit_query_grid_stage(rt_navmesh3d *nm, navmesh3d_query_grid_stage *stage) {
    if (!nm || !stage)
        return;
    navmesh3d_free_query_grid(nm);
    nm->qgrid_min_x = stage->min_x;
    nm->qgrid_min_z = stage->min_z;
    nm->qgrid_inv_cell = stage->inv_cell;
    nm->qgrid_nx = stage->nx;
    nm->qgrid_nz = stage->nz;
    nm->qgrid_starts = stage->starts;
    nm->qgrid_tris = stage->tris;
    memset(stage, 0, sizeof(*stage));
}

/// @brief Convert a finite grid-space coordinate to a clamped cell index without narrowing UB.
/// @param value World-space coordinate on one grid axis.
/// @param minimum World-space minimum of that axis.
/// @param inv Reciprocal cell size.
/// @param dimension Positive number of grid cells on the axis.
/// @return An index in `[0, dimension - 1]`.
static int32_t navmesh3d_grid_axis_clamped(double value,
                                           double minimum,
                                           double inv,
                                           int32_t dimension) {
    double scaled = (value - minimum) * inv;
    if (!isfinite(scaled) || scaled <= 0.0)
        return 0;
    if (scaled >= (double)dimension)
        return dimension - 1;
    return (int32_t)scaled;
}

/// @brief Convert a world coordinate to an in-bounds cell without a narrowing conversion first.
/// @param value World-space coordinate on one grid axis.
/// @param minimum World-space grid origin for the axis.
/// @param inv Reciprocal cell size.
/// @param dimension Positive number of cells along the axis.
/// @param out_index Receives a value in `[0, dimension - 1]` on success.
/// @return 1 when the coordinate lies in the grid, otherwise 0 with @p out_index untouched.
static int navmesh3d_grid_axis_checked(
    double value, double minimum, double inv, int32_t dimension, int32_t *out_index) {
    if (!out_index || dimension <= 0 || !isfinite(value) || !isfinite(minimum) || !isfinite(inv) ||
        inv <= 0.0)
        return 0;
    double scaled = (value - minimum) * inv;
    if (!isfinite(scaled) || scaled < 0.0 || scaled >= (double)dimension)
        return 0;
    *out_index = (int32_t)scaled;
    return 1;
}

/// @brief Compute the inclusive grid-cell range covered by one triangle's XZ bounding box.
/// @param nm Navigation mesh supplying the vertex array.
/// @param t Triangle whose vertex indices and bounds are validated.
/// @param min_x Grid X origin.
/// @param min_z Grid Z origin.
/// @param inv Reciprocal grid-cell size.
/// @param gnx Grid width in cells.
/// @param gnz Grid depth in cells.
/// @param cx0 Output minimum X cell.
/// @param cx1 Output maximum X cell.
/// @param cz0 Output minimum Z cell.
/// @param cz1 Output maximum Z cell.
/// @return 1 when the triangle is valid and all outputs were written, otherwise 0.
static int navmesh3d_tri_cell_range(const rt_navmesh3d *nm,
                                    const nav_triangle_t *t,
                                    double min_x,
                                    double min_z,
                                    double inv,
                                    int32_t gnx,
                                    int32_t gnz,
                                    int32_t *cx0,
                                    int32_t *cx1,
                                    int32_t *cz0,
                                    int32_t *cz1) {
    if (!nm || !t || !nm->vertices || gnx <= 0 || gnz <= 0 || !cx0 || !cx1 || !cz0 || !cz1)
        return 0;
    for (int k = 0; k < 3; k++)
        if (t->v[k] < 0 || t->v[k] >= nm->vertex_count)
            return 0;
    const float *a = nm->vertices[t->v[0]].position;
    const float *b = nm->vertices[t->v[1]].position;
    const float *c = nm->vertices[t->v[2]].position;
    if (!navmesh3d_float3_finite(a) || !navmesh3d_float3_finite(b) || !navmesh3d_float3_finite(c))
        return 0;
    *cx0 = navmesh3d_grid_axis_clamped((double)fminf(a[0], fminf(b[0], c[0])), min_x, inv, gnx);
    *cx1 = navmesh3d_grid_axis_clamped((double)fmaxf(a[0], fmaxf(b[0], c[0])), min_x, inv, gnx);
    *cz0 = navmesh3d_grid_axis_clamped((double)fminf(a[2], fminf(b[2], c[2])), min_z, inv, gnz);
    *cz1 = navmesh3d_grid_axis_clamped((double)fmaxf(a[2], fmaxf(b[2], c[2])), min_z, inv, gnz);
    return 1;
}

#include "rt_navmesh3d_query_grid_stage.inc"

/// @brief Rebuild the live uniform query grid through stage-and-swap publication.
/// @details Allocation failure retains the prior grid. A checked replication-budget refusal
///   publishes the correct empty-grid representation so queries use the linear scan.
/// @param nm Navigation mesh whose current triangles are indexed.
/// @return 1 when a grid or bounded fallback was published, otherwise 0 with prior state retained.
static int navmesh3d_build_query_grid(rt_navmesh3d *nm) {
    navmesh3d_query_grid_stage stage;
    if (!nm)
        return 0;
    navmesh3d_query_grid_result result =
        navmesh3d_stage_query_grid(nm, nm->triangles, nm->triangle_count, &stage);
    if (result == NAVMESH3D_QUERY_GRID_ERROR) {
        navmesh3d_discard_query_grid_stage(&stage);
        navmesh3d_record_query_grid_fallback(nm);
        return 0;
    }
    if (result == NAVMESH3D_QUERY_GRID_BOUNDED_FALLBACK)
        navmesh3d_record_query_grid_fallback(nm);
    navmesh3d_commit_query_grid_stage(nm, &stage);
    return 1;
}

/// @brief Rebuild the walkable triangle set from the source triangles, dropping over-steep ones.
/// @details A triangle is walkable when its upward normal's angle from vertical is within
///   `nm->max_slope` (compared via cosines). Candidate triangles, adjacency, and query-grid arrays
///   are completed before publication. An initial build may publish the bounded linear fallback;
///   later allocation failure leaves the previous triangles and grid bit-identical.
/// @param nm Navigation mesh with preserved source triangles and destination capacity.
/// @return 1 after one complete representation is published, or 0 with prior live topology intact.
static int navmesh3d_apply_slope_filter(rt_navmesh3d *nm) {
    if (!nm || !nm->source_triangles || !nm->triangles)
        return 0;
    if (nm->source_triangle_count < 0 ||
        (size_t)nm->source_triangle_count > SIZE_MAX / sizeof(nav_triangle_t))
        return 0;
    size_t stage_count = nm->source_triangle_count > 0 ? (size_t)nm->source_triangle_count : 1u;
    nav_triangle_t *staged = (nav_triangle_t *)malloc(stage_count * sizeof(nav_triangle_t));
    if (!staged)
        return 0;
    double max_slope_cos = cos(nm->max_slope * M_PI / 180.0);
    int32_t staged_count = 0;
    for (int32_t i = 0; i < nm->source_triangle_count; i++) {
        nav_triangle_t tri = nm->source_triangles[i];
        if (tri.normal[1] < (float)max_slope_cos)
            continue;
        tri.neighbors[0] = tri.neighbors[1] = tri.neighbors[2] = -1;
        /* Keep unwalkable/source-carved and obstacle-carved triangles in the set but flag them
         * blocked, so a per-tile rebuild can flip flags/geometry without recompacting arrays. */
        tri.blocked = (tri.blocked || navmesh3d_triangle_blocked_by_obstacle(nm, &tri)) ? 1 : 0;
        tri.traversal_cost = navmesh3d_sanitize_traversal_cost((double)tri.traversal_cost);
        staged[staged_count++] = tri;
    }
    if (!navmesh3d_build_adjacency_for(nm, staged, staged_count)) {
        free(staged);
        return 0;
    }
    navmesh3d_query_grid_stage grid_stage;
    navmesh3d_query_grid_result grid_result =
        navmesh3d_stage_query_grid(nm, staged, staged_count, &grid_stage);
    if (grid_result != NAVMESH3D_QUERY_GRID_READY)
        navmesh3d_record_query_grid_fallback(nm);
    if (grid_result == NAVMESH3D_QUERY_GRID_ERROR && nm->topology_revision != 0u) {
        navmesh3d_discard_query_grid_stage(&grid_stage);
        free(staged);
        return 0;
    }
    memcpy(nm->triangles, staged, (size_t)staged_count * sizeof(nav_triangle_t));
    nm->triangle_count = staged_count;
    free(staged);
    navmesh3d_commit_query_grid_stage(nm, &grid_stage);
    if (nm->topology_revision != UINT64_MAX)
        nm->topology_revision++;
    navmesh3d_refresh_offmesh_links(nm);
    return 1;
}

/// @brief Map a world-space XZ position to its tile coordinates under the tiled-bake convention.
/// @details Tile (tx,tz) spans [origin + t*tile_size, origin + (t+1)*tile_size) where the origin is
///          the query-grid minimum (the mesh's min XZ). Floor keeps negative coordinates on the
///          correct side of the origin. Returns 0 (outputs untouched) when the mesh is not tiled or
///          has no spatial grid yet.
/// @param nm Borrowed tiled navmesh.
/// @param px World X coordinate.
/// @param pz World Z coordinate.
/// @param out_tx Optional output receiving the signed X tile coordinate.
/// @param out_tz Optional output receiving the signed Z tile coordinate.
/// @return 1 for a valid tiled mapping, otherwise 0.
static int navmesh3d_tile_of_point(
    const rt_navmesh3d *nm, double px, double pz, int64_t *out_tx, int64_t *out_tz) {
    if (!nm || !isfinite(nm->tile_size) || nm->tile_size < NAVMESH3D_CELL_SIZE_MIN ||
        nm->tile_size > NAVMESH3D_TILE_SIZE_MAX || nm->qgrid_nx <= 0 || !isfinite(px) ||
        !isfinite(pz) || !isfinite(nm->qgrid_min_x) || !isfinite(nm->qgrid_min_z))
        return 0;
    double inv = 1.0 / nm->tile_size;
    double tx_value = floor((px - nm->qgrid_min_x) * inv);
    double tz_value = floor((pz - nm->qgrid_min_z) * inv);
    if (!isfinite(tx_value) || !isfinite(tz_value) ||
        tx_value < -(double)NAVMESH3D_TILE_INDEX_ABS_MAX ||
        tx_value > (double)NAVMESH3D_TILE_INDEX_ABS_MAX ||
        tz_value < -(double)NAVMESH3D_TILE_INDEX_ABS_MAX ||
        tz_value > (double)NAVMESH3D_TILE_INDEX_ABS_MAX)
        return 0;
    if (out_tx)
        *out_tx = (int64_t)tx_value;
    if (out_tz)
        *out_tz = (int64_t)tz_value;
    return 1;
}

/// @brief Re-evaluate obstacle blocking for every live triangle.
/// @param nm Mutable navmesh whose triangle flags and off-mesh endpoints are refreshed.
static void navmesh3d_reflag_all_triangles(rt_navmesh3d *nm) {
    if (!nm || !nm->triangles)
        return;
    for (int32_t i = 0; i < nm->triangle_count; i++) {
        nav_triangle_t *t = &nm->triangles[i];
        int8_t source_blocked = (nm->source_triangles && i < nm->source_triangle_count &&
                                 nm->source_triangles[i].blocked)
                                    ? 1
                                    : 0;
        t->blocked = (source_blocked || navmesh3d_triangle_blocked_by_obstacle(nm, t)) ? 1 : 0;
    }
    navmesh3d_refresh_offmesh_links(nm);
}

/// @brief Re-evaluate obstacle blocking for every triangle whose centroid lies in tile (tx,tz).
/// @details The O(tile) heart of tile-local rebuild: flips each in-tile triangle's `blocked` flag
///          against the current obstacle list without touching adjacency, the query grid, or any
///          other tile. Triangles outside the tile keep whatever state a prior edit left them in,
///          which is exactly what lets one tile be re-carved while the rest of the mesh is stale.
///          Returns the number of triangles re-flagged.
/// @param nm Mutable tiled navmesh.
/// @param tile_x Signed X tile coordinate.
/// @param tile_z Signed Z tile coordinate.
/// @return Number of triangle flags reevaluated.
static int32_t navmesh3d_reflag_tile(rt_navmesh3d *nm, int64_t tile_x, int64_t tile_z) {
    if (!nm || !nm->triangles || nm->tile_size <= 0.0 || nm->qgrid_nx <= 0)
        return 0;
    if (!navmesh3d_tile_index_valid(tile_x) || !navmesh3d_tile_index_valid(tile_z))
        return 0;
    double tsz = nm->tile_size;
    double x0 = nm->qgrid_min_x + (double)tile_x * tsz;
    double x1 = x0 + tsz;
    double z0 = nm->qgrid_min_z + (double)tile_z * tsz;
    double z1 = z0 + tsz;
    int32_t touched = 0;
    for (int32_t i = 0; i < nm->triangle_count; i++) {
        nav_triangle_t *t = &nm->triangles[i];
        double cx = (double)t->centroid[0];
        double cz = (double)t->centroid[2];
        if (cx < x0 || cx >= x1 || cz < z0 || cz >= z1)
            continue;
        int8_t source_blocked = (nm->source_triangles && i < nm->source_triangle_count &&
                                 nm->source_triangles[i].blocked)
                                    ? 1
                                    : 0;
        t->blocked = (source_blocked || navmesh3d_triangle_blocked_by_obstacle(nm, t)) ? 1 : 0;
        touched++;
    }
    return touched;
}

/// @brief Re-evaluate dynamic obstacle blocking for all tiles overlapped by an AABB.
/// @param nm Mutable tiled navmesh.
/// @param omin Borrowed obstacle minimum corner.
/// @param omax Borrowed obstacle maximum corner.
static void navmesh3d_reflag_obstacle_tiles(rt_navmesh3d *nm,
                                            const float *omin,
                                            const float *omax) {
    if (!nm || !omin || !omax || nm->tile_size <= 0.0 || nm->qgrid_nx <= 0)
        return;
    int64_t tx0 = 0, tz0 = 0, tx1 = 0, tz1 = 0;
    if (!navmesh3d_tile_of_point(nm, (double)omin[0], (double)omin[2], &tx0, &tz0) ||
        !navmesh3d_tile_of_point(nm, (double)omax[0], (double)omax[2], &tx1, &tz1)) {
        navmesh3d_reflag_all_triangles(nm);
        return;
    }
    if (tx1 < tx0) {
        int64_t t = tx0;
        tx0 = tx1;
        tx1 = t;
    }
    if (tz1 < tz0) {
        int64_t t = tz0;
        tz0 = tz1;
        tz1 = t;
    }
    int64_t tile_w = tx1 - tx0 + 1;
    int64_t tile_h = tz1 - tz0 + 1;
    if (tile_w <= 0 || tile_h <= 0 || tile_w > NAVMESH3D_MAX_TILES_PER_REFLAG ||
        tile_h > NAVMESH3D_MAX_TILES_PER_REFLAG ||
        tile_w > NAVMESH3D_MAX_TILES_PER_REFLAG / tile_h) {
        navmesh3d_reflag_all_triangles(nm);
        return;
    }
    for (int64_t tz = tz0; tz <= tz1; tz++)
        for (int64_t tx = tx0; tx <= tx1; tx++)
            navmesh3d_reflag_tile(nm, tx, tz);
    navmesh3d_refresh_offmesh_links(nm);
}

/// @brief True when the navmesh retained fixed-grid voxel source data for tile rebuilds.
/// @param nm Borrowed navmesh.
/// @return Non-zero when all required retained voxel and topology arrays are available.
static int navmesh3d_has_voxel_source(const rt_navmesh3d *nm) {
    return nm && nm->voxel_cell_height && nm->voxel_cell_walkable && nm->voxel_corner_vertices &&
           nm->voxel_nx > 0 && nm->voxel_nz > 0 && nm->voxel_cell_size > 0.0 && nm->vertices &&
           nm->source_triangles && nm->triangles;
}

/// @brief Return the retained voxel cell index, or SIZE_MAX for an invalid coordinate.
/// @param nm Borrowed navmesh with retained voxel source.
/// @param cx X cell coordinate.
/// @param cz Z cell coordinate.
/// @return Linear cell index, or @c SIZE_MAX.
static size_t navmesh3d_voxel_cell_index(const rt_navmesh3d *nm, int32_t cx, int32_t cz) {
    if (!navmesh3d_has_voxel_source(nm) || cx < 0 || cz < 0 || cx >= nm->voxel_nx ||
        cz >= nm->voxel_nz)
        return SIZE_MAX;
    return (size_t)cz * (size_t)nm->voxel_nx + (size_t)cx;
}

/// @brief Get the fixed source-triangle index pair for a retained voxel cell.
/// @param nm Borrowed navmesh with fixed two-triangle-per-cell topology.
/// @param cx X cell coordinate.
/// @param cz Z cell coordinate.
/// @return Index of the cell's first triangle, or -1.
static int32_t navmesh3d_voxel_cell_tri_start(const rt_navmesh3d *nm, int32_t cx, int32_t cz) {
    size_t cell = navmesh3d_voxel_cell_index(nm, cx, cz);
    if (cell == SIZE_MAX || cell > (size_t)INT32_MAX / 2)
        return -1;
    int32_t start = (int32_t)(cell * 2u);
    if (start < 0 || start + 1 >= nm->source_triangle_count || start + 1 >= nm->triangle_count)
        return -1;
    return start;
}

/// @brief Recompute one retained voxel corner height from the current tile source cells.
/// @param nm Mutable navmesh with retained voxel source.
/// @param ccx X corner coordinate.
/// @param ccz Z corner coordinate.
static void navmesh3d_voxel_refresh_corner(rt_navmesh3d *nm, int32_t ccx, int32_t ccz) {
    if (!navmesh3d_has_voxel_source(nm) || ccx < 0 || ccz < 0 || ccx > nm->voxel_nx ||
        ccz > nm->voxel_nz)
        return;
    int32_t cnx = nm->voxel_nx + 1;
    size_t corner = (size_t)ccz * (size_t)cnx + (size_t)ccx;
    int32_t vid = nm->voxel_corner_vertices[corner];
    if (vid < 0 || vid >= nm->vertex_count)
        return;
    double sum = 0.0;
    int n = 0;
    for (int dz = -1; dz <= 0; dz++) {
        for (int dx = -1; dx <= 0; dx++) {
            int32_t sx = ccx + dx;
            int32_t sz = ccz + dz;
            size_t cell = navmesh3d_voxel_cell_index(nm, sx, sz);
            if (cell == SIZE_MAX || !nm->voxel_cell_walkable[cell])
                continue;
            sum += (double)nm->voxel_cell_height[cell];
            n++;
        }
    }
    nm->vertices[vid].position[1] = n > 0 ? (float)(sum / (double)n) : 0.0f;
}

/// @brief Recompute one retained voxel cell's two source/current triangles from fixed corners.
/// @param nm Mutable navmesh with retained fixed-grid topology.
/// @param cx X cell coordinate.
/// @param cz Z cell coordinate.
static void navmesh3d_voxel_refresh_cell(rt_navmesh3d *nm, int32_t cx, int32_t cz) {
    int32_t tri_start = navmesh3d_voxel_cell_tri_start(nm, cx, cz);
    if (tri_start < 0)
        return;
    int32_t cnx = nm->voxel_nx + 1;
    int32_t a = nm->voxel_corner_vertices[(size_t)cz * (size_t)cnx + (size_t)cx];
    int32_t b = nm->voxel_corner_vertices[(size_t)cz * (size_t)cnx + (size_t)(cx + 1)];
    int32_t c = nm->voxel_corner_vertices[(size_t)(cz + 1) * (size_t)cnx + (size_t)(cx + 1)];
    int32_t d = nm->voxel_corner_vertices[(size_t)(cz + 1) * (size_t)cnx + (size_t)cx];
    if (a < 0 || b < 0 || c < 0 || d < 0)
        return;
    size_t cell = navmesh3d_voxel_cell_index(nm, cx, cz);
    int8_t source_blocked = (cell == SIZE_MAX || !nm->voxel_cell_walkable[cell]) ? 1 : 0;
    int32_t old_source_count = nm->source_triangle_count;
    nav_triangle_t updated[2];
    int32_t old_area[2] = {nm->source_triangles[tri_start].area_id,
                           nm->source_triangles[tri_start + 1].area_id};
    float old_cost[2] = {nm->source_triangles[tri_start].traversal_cost,
                         nm->source_triangles[tri_start + 1].traversal_cost};
    nm->source_triangle_count = tri_start;
    navmesh3d_voxel_emit_tri(nm, a, c, b, source_blocked);
    navmesh3d_voxel_emit_tri(nm, a, d, c, source_blocked);
    updated[0] = nm->source_triangles[tri_start];
    updated[1] = nm->source_triangles[tri_start + 1];
    nm->source_triangle_count = old_source_count;
    for (int i = 0; i < 2; i++) {
        nav_triangle_t tri = updated[i];
        tri.area_id = old_area[i];
        tri.traversal_cost = navmesh3d_sanitize_traversal_cost((double)old_cost[i]);
        updated[i].area_id = tri.area_id;
        updated[i].traversal_cost = tri.traversal_cost;
        tri.neighbors[0] = nm->triangles[tri_start + i].neighbors[0];
        tri.neighbors[1] = nm->triangles[tri_start + i].neighbors[1];
        tri.neighbors[2] = nm->triangles[tri_start + i].neighbors[2];
        nm->source_triangles[tri_start + i] = updated[i];
        tri.blocked = (source_blocked || navmesh3d_triangle_blocked_by_obstacle(nm, &tri)) ? 1 : 0;
        nm->triangles[tri_start + i] = tri;
    }
}

/// @brief Compute the retained voxel-cell range whose centers lie in tile (tile_x,tile_z).
/// @param nm Borrowed tiled navmesh with retained voxel source.
/// @param tile_x Signed X tile coordinate.
/// @param tile_z Signed Z tile coordinate.
/// @param out_cx0 Optional output receiving minimum X cell.
/// @param out_cx1 Optional output receiving maximum X cell.
/// @param out_cz0 Optional output receiving minimum Z cell.
/// @param out_cz1 Optional output receiving maximum Z cell.
/// @return 1 for a non-empty in-bounds cell range, otherwise 0.
static int navmesh3d_voxel_tile_cell_range(const rt_navmesh3d *nm,
                                           int64_t tile_x,
                                           int64_t tile_z,
                                           int32_t *out_cx0,
                                           int32_t *out_cx1,
                                           int32_t *out_cz0,
                                           int32_t *out_cz1) {
    if (!navmesh3d_has_voxel_source(nm) || nm->tile_size <= 0.0)
        return 0;
    if (!navmesh3d_tile_index_valid(tile_x) || !navmesh3d_tile_index_valid(tile_z))
        return 0;
    double x0 = nm->qgrid_min_x + (double)tile_x * nm->tile_size;
    double x1 = x0 + nm->tile_size;
    double z0 = nm->qgrid_min_z + (double)tile_z * nm->tile_size;
    double z1 = z0 + nm->tile_size;
    int32_t cx0 =
        (int32_t)ceil((x0 - nm->voxel_min_x - nm->voxel_cell_size * 0.5) / nm->voxel_cell_size);
    int32_t cx1 =
        (int32_t)floor((x1 - nm->voxel_min_x - nm->voxel_cell_size * 0.5) / nm->voxel_cell_size);
    int32_t cz0 =
        (int32_t)ceil((z0 - nm->voxel_min_z - nm->voxel_cell_size * 0.5) / nm->voxel_cell_size);
    int32_t cz1 =
        (int32_t)floor((z1 - nm->voxel_min_z - nm->voxel_cell_size * 0.5) / nm->voxel_cell_size);
    if (cx0 < 0)
        cx0 = 0;
    if (cz0 < 0)
        cz0 = 0;
    if (cx1 >= nm->voxel_nx)
        cx1 = nm->voxel_nx - 1;
    if (cz1 >= nm->voxel_nz)
        cz1 = nm->voxel_nz - 1;
    if (cx1 < cx0 || cz1 < cz0)
        return 0;
    if (out_cx0)
        *out_cx0 = cx0;
    if (out_cx1)
        *out_cx1 = cx1;
    if (out_cz0)
        *out_cz0 = cz0;
    if (out_cz1)
        *out_cz1 = cz1;
    return 1;
}

/// @brief Rebuild one retained voxel tile's geometry/blocked flags in place.
/// @param nm Mutable tiled navmesh with retained voxel source.
/// @param tile_x Signed X tile coordinate.
/// @param tile_z Signed Z tile coordinate.
/// @return Number of voxel cells rebuilt.
static int32_t navmesh3d_rebuild_voxel_tile(rt_navmesh3d *nm, int64_t tile_x, int64_t tile_z) {
    int32_t cx0, cx1, cz0, cz1;
    int32_t touched = 0;
    if (!navmesh3d_voxel_tile_cell_range(nm, tile_x, tile_z, &cx0, &cx1, &cz0, &cz1))
        return 0;
    for (int32_t cz = cz0; cz <= cz1 + 1; cz++)
        for (int32_t cx = cx0; cx <= cx1 + 1; cx++)
            navmesh3d_voxel_refresh_corner(nm, cx, cz);
    for (int32_t cz = cz0; cz <= cz1; cz++) {
        for (int32_t cx = cx0; cx <= cx1; cx++) {
            navmesh3d_voxel_refresh_cell(nm, cx, cz);
            touched++;
        }
    }
    navmesh3d_refresh_offmesh_links(nm);
    return touched;
}

/// @brief Read one authoritative finite mesh position into the navigation coordinate range.
/// @param mesh Borrowed mesh with repaired geometry counts.
/// @param index Vertex index below the mesh's safe vertex count.
/// @param out Caller-owned three-double destination.
/// @return Non-zero when the authoritative position is finite, otherwise zero.
static int navmesh3d_read_mesh_position(const rt_mesh3d *mesh, uint32_t index, double *out) {
    if (!mesh || !out || index >= rt_mesh3d_safe_vertex_count(mesh))
        return 0;
    for (int lane = 0; lane < 3; lane++) {
        double value = mesh->positions64 ? mesh->positions64[(size_t)index * 3u + (size_t)lane]
                                         : (double)mesh->vertices[index].pos[lane];
        if (!isfinite(value))
            return 0;
        out[lane] = navmesh3d_coord_or(value, 0.0);
    }
    return 1;
}

/// @brief Transform a point by a row-major Mat4.
/// @param m Borrowed 16-element row-major transform.
/// @param p Borrowed three-double point.
/// @param out Caller-owned three-double transformed point.
static void navmesh3d_transform_point(const double *m, const double *p, double *out) {
    out[0] = navmesh3d_coord_or(m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], 0.0);
    out[1] = navmesh3d_coord_or(m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7], 0.0);
    out[2] = navmesh3d_coord_or(m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11], 0.0);
}

/// @brief Transform and normalize a normal by the linear basis's inverse transpose.
/// @details The cofactor transform is proportional to inverse-transpose; its determinant sign is
///          removed so reflected scene nodes retain their authored outward-facing normal. Unlike
///          a direct linear transform, it remains correct for non-uniform scale.
/// @param m Borrowed 16-element row-major transform.
/// @param n Borrowed three-float normal.
/// @param out Caller-owned normalized three-double result; degenerate input becomes positive Y.
static void navmesh3d_transform_normal(const double *m, const float *n, double *out) {
    double c00 = m[5] * m[10] - m[6] * m[9];
    double c01 = m[6] * m[8] - m[4] * m[10];
    double c02 = m[4] * m[9] - m[5] * m[8];
    double c10 = m[2] * m[9] - m[1] * m[10];
    double c11 = m[0] * m[10] - m[2] * m[8];
    double c12 = m[1] * m[8] - m[0] * m[9];
    double c20 = m[1] * m[6] - m[2] * m[5];
    double c21 = m[2] * m[4] - m[0] * m[6];
    double c22 = m[0] * m[5] - m[1] * m[4];
    out[0] = c00 * (double)n[0] + c01 * (double)n[1] + c02 * (double)n[2];
    out[1] = c10 * (double)n[0] + c11 * (double)n[1] + c12 * (double)n[2];
    out[2] = c20 * (double)n[0] + c21 * (double)n[1] + c22 * (double)n[2];
    double determinant = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (isfinite(determinant) && determinant < 0.0) {
        out[0] = -out[0];
        out[1] = -out[1];
        out[2] = -out[2];
    }
    double len = hypot(hypot(out[0], out[1]), out[2]);
    if (!isfinite(len) || len <= 1e-12) {
        out[0] = 0.0;
        out[1] = 1.0;
        out[2] = 0.0;
        return;
    }
    out[0] /= len;
    out[1] /= len;
    out[2] /= len;
}

/// @brief Append one SceneNode3D mesh into a temporary world-space Mesh3D.
/// @param dst Mutable temporary destination mesh.
/// @param node Borrowed scene node whose optional mesh is transformed and appended.
/// @return 1 when the node is empty or appended successfully, otherwise 0.
static int navmesh3d_append_scene_node_mesh(rt_mesh3d *dst, rt_scene_node3d *node) {
    rt_mesh3d *src;
    void *world_obj;
    double world[16];
    uint32_t base_vertex;
    uint32_t src_vertex_count;
    uint32_t src_index_count;
    int success = 1;
    if (!dst || !node || !node->mesh)
        return 1;
    src = (rt_mesh3d *)rt_g3d_checked_or_null(node->mesh, RT_G3D_MESH3D_CLASS_ID);
    if (!src)
        return 1;
    rt_mesh3d_repair_geometry_counts(src);
    rt_mesh3d_repair_geometry_counts(dst);
    src_vertex_count = rt_mesh3d_safe_vertex_count(src);
    src_index_count = rt_mesh3d_safe_index_count(src);
    if (src_vertex_count == 0 || src_index_count < 3)
        return 1;
    if (dst->vertex_count > UINT32_MAX - src_vertex_count ||
        dst->index_count > UINT32_MAX - src_index_count)
        return 0;
    world_obj = rt_scene_node3d_get_world_matrix(node);
    if (!world_obj)
        return 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            double value = rt_mat4_get(world_obj, r, c);
            if (!isfinite(value)) {
                navmesh3d_release_local(world_obj);
                return 0;
            }
            world[r * 4 + c] = navmesh3d_coord_or(value, 0.0);
        }
    }
    navmesh3d_release_local(world_obj);

    base_vertex = dst->vertex_count;
    rt_mesh3d_reserve(dst,
                      (int64_t)dst->vertex_count + (int64_t)src_vertex_count,
                      ((int64_t)dst->index_count + (int64_t)src_index_count) / 3);
    if (dst->vertex_capacity < dst->vertex_count + src_vertex_count ||
        dst->index_capacity < dst->index_count + src_index_count)
        return 0;
    rt_mesh3d_begin_geometry_batch(dst);
    for (uint32_t i = 0; i < src_vertex_count; i++) {
        double local_pos[3];
        double pos[3];
        double normal[3];
        float authored_normal[3] = {
            src->vertices[i].normal[0], src->vertices[i].normal[1], src->vertices[i].normal[2]};
        if (!navmesh3d_read_mesh_position(src, i, local_pos)) {
            success = 0;
            break;
        }
        if (!navmesh3d_float3_finite(authored_normal)) {
            authored_normal[0] = 0.0f;
            authored_normal[1] = 1.0f;
            authored_normal[2] = 0.0f;
        }
        navmesh3d_transform_point(world, local_pos, pos);
        navmesh3d_transform_normal(world, authored_normal, normal);
        rt_mesh3d_add_vertex(dst,
                             pos[0],
                             pos[1],
                             pos[2],
                             normal[0],
                             normal[1],
                             normal[2],
                             (double)src->vertices[i].uv[0],
                             (double)src->vertices[i].uv[1]);
        if (dst->build_failed) {
            success = 0;
            break;
        }
    }
    for (uint32_t i = 0; success && i + 2 < src_index_count; i += 3) {
        uint32_t i0 = src->indices[i];
        uint32_t i1 = src->indices[i + 1];
        uint32_t i2 = src->indices[i + 2];
        if (i0 >= src_vertex_count || i1 >= src_vertex_count || i2 >= src_vertex_count)
            continue;
        rt_mesh3d_add_triangle(dst,
                               (int64_t)base_vertex + (int64_t)i0,
                               (int64_t)base_vertex + (int64_t)i1,
                               (int64_t)base_vertex + (int64_t)i2);
        if (dst->build_failed)
            success = 0;
    }
    rt_mesh3d_end_geometry_batch(dst);
    return success;
}

/// @brief Gather all Mesh3D-bearing scene nodes into a temporary world-space Mesh3D.
/// @param scene_obj Borrowed Scene3D handle.
/// @return New GC-managed aggregate Mesh3D, or null when no usable geometry exists or collection
///         fails.
static void *navmesh3d_build_scene_source_mesh(void *scene_obj) {
    rt_scene3d *scene = (rt_scene3d *)rt_g3d_checked_or_null(scene_obj, RT_G3D_SCENE3D_CLASS_ID);
    rt_mesh3d *mesh;
    rt_scene_node3d **stack = NULL;
    int32_t stack_count = 0;
    int32_t stack_capacity = 64;
    int32_t appended = 0;
    if (!scene || !scene->root)
        return NULL;
    mesh = (rt_mesh3d *)rt_mesh3d_new();
    if (!mesh)
        return NULL;
    stack = (rt_scene_node3d **)malloc((size_t)stack_capacity * sizeof(*stack));
    if (!stack) {
        navmesh3d_release_local(mesh);
        return NULL;
    }
    stack[stack_count++] = scene->root;
    while (stack_count > 0) {
        rt_scene_node3d *node = stack[--stack_count];
        if (!node)
            continue;
        if (node->mesh) {
            uint32_t before = mesh->index_count;
            if (!navmesh3d_append_scene_node_mesh(mesh, node)) {
                free(stack);
                navmesh3d_release_local(mesh);
                return NULL;
            }
            if (mesh->index_count > before)
                appended = 1;
        }
        int32_t child_count = scene3d_node_child_count(node);
        for (int32_t i = 0; i < child_count; i++) {
            if (stack_count >= stack_capacity) {
                int32_t next_capacity;
                rt_scene_node3d **grown;
                if (stack_capacity > INT32_MAX / 2) {
                    free(stack);
                    navmesh3d_release_local(mesh);
                    return NULL;
                }
                next_capacity = stack_capacity * 2;
                if ((size_t)next_capacity > SIZE_MAX / sizeof(rt_scene_node3d *)) {
                    free(stack);
                    navmesh3d_release_local(mesh);
                    return NULL;
                }
                grown = (rt_scene_node3d **)realloc(stack, (size_t)next_capacity * sizeof(*stack));
                if (!grown) {
                    free(stack);
                    navmesh3d_release_local(mesh);
                    return NULL;
                }
                stack = grown;
                stack_capacity = next_capacity;
            }
            stack[stack_count++] = node->children[i];
        }
    }
    free(stack);
    if (!appended || mesh->vertex_count == 0 || mesh->index_count < 3) {
        navmesh3d_release_local(mesh);
        return NULL;
    }
    return mesh;
}

/// @brief Bake a navigation mesh from a triangle mesh. Filters out triangles whose normal
/// exceeds the default 45° slope (re-configurable via `_set_max_slope`). Stores agent radius/
/// height for later edge-pull-in safety. Returns the navmesh handle, or NULL on alloc failure
/// / degenerate input mesh.
/// @param mesh_obj Borrowed Mesh3D source.
/// @param agent_radius Requested positive clearance radius.
/// @param agent_height Requested positive agent height.
/// @return New GC-managed NavMesh3D, or null for invalid, empty, oversized, or failed input.
void *rt_navmesh3d_build(void *mesh_obj, double agent_radius, double agent_height) {
    if (!mesh_obj)
        return NULL;
    rt_mesh3d *m = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
    if (!m)
        return NULL;
    rt_mesh3d_repair_geometry_counts(m);
    uint32_t vertex_count = rt_mesh3d_safe_vertex_count(m);
    uint32_t index_count = rt_mesh3d_safe_index_count(m);
    if (vertex_count == 0 || index_count < 3)
        return NULL;
    if (vertex_count > INT32_MAX || index_count > INT32_MAX || (index_count / 3u) > INT32_MAX) {
        rt_trap("NavMesh3D.Build: mesh is too large");
        return NULL;
    }
    for (uint32_t i = 0; i < vertex_count; i++) {
        double position[3];
        if (!navmesh3d_read_mesh_position(m, i, position))
            return NULL;
    }

    rt_navmesh3d *nm =
        (rt_navmesh3d *)rt_obj_new_i64(RT_G3D_NAVMESH3D_CLASS_ID, (int64_t)sizeof(rt_navmesh3d));
    if (!nm) {
        rt_trap("NavMesh3D.Build: allocation failed");
        return NULL;
    }
    memset(nm, 0, sizeof(*nm));
    nm->vptr = NULL;
    nm->agent_radius = navmesh3d_agent_dim_or(agent_radius, 0.4);
    nm->agent_height = navmesh3d_agent_dim_or(agent_height, 1.8);
    nm->max_slope = navmesh3d_sanitize_slope(45.0);
    rt_obj_set_finalizer(nm, navmesh3d_finalizer);
    if (!navmesh3d_init_path_workspace_gate(nm)) {
        navmesh3d_free_partial(nm);
        rt_trap("NavMesh3D.Build: workspace gate allocation failed");
        return NULL;
    }

    /* Phase 1: Copy vertices */
    nm->vertex_count = (int32_t)vertex_count;
    nm->vertices = (nav_vertex_t *)malloc((size_t)nm->vertex_count * sizeof(nav_vertex_t));
    if (!nm->vertices) {
        navmesh3d_free_partial(nm);
        rt_trap("NavMesh3D.Build: vertex allocation failed");
        return NULL;
    }
    for (uint32_t i = 0; i < vertex_count; i++) {
        double position[3];
        (void)navmesh3d_read_mesh_position(m, i, position);
        nm->vertices[i].position[0] = (float)position[0];
        nm->vertices[i].position[1] = (float)position[1];
        nm->vertices[i].position[2] = (float)position[2];
    }

    /* Phase 2: Copy non-degenerate source triangles; walkable filtering is
     * applied from this preserved set so SetMaxSlope can rebuild in-place. */
    int32_t tri_cap = (int32_t)(index_count / 3);
    nm->source_triangles = (nav_triangle_t *)malloc((size_t)tri_cap * sizeof(nav_triangle_t));
    nm->triangles = (nav_triangle_t *)malloc((size_t)tri_cap * sizeof(nav_triangle_t));
    if (!nm->source_triangles || !nm->triangles) {
        navmesh3d_free_partial(nm);
        rt_trap("NavMesh3D.Build: triangle allocation failed");
        return NULL;
    }
    nm->source_triangle_count = 0;
    nm->triangle_count = 0;

    for (uint32_t i = 0; i + 2 < index_count; i += 3) {
        uint32_t i0 = m->indices[i], i1 = m->indices[i + 1], i2 = m->indices[i + 2];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
            continue;

        float *p0 = nm->vertices[i0].position;
        float *p1 = nm->vertices[i1].position;
        float *p2 = nm->vertices[i2].position;

        /* Face normal via cross product */
        double e1x = (double)p1[0] - (double)p0[0];
        double e1y = (double)p1[1] - (double)p0[1];
        double e1z = (double)p1[2] - (double)p0[2];
        double e2x = (double)p2[0] - (double)p0[0];
        double e2y = (double)p2[1] - (double)p0[1];
        double e2z = (double)p2[2] - (double)p0[2];
        double nx = e1y * e2z - e1z * e2y;
        double ny = e1z * e2x - e1x * e2z;
        double nz = e1x * e2y - e1y * e2x;
        double nlen = hypot(hypot(nx, ny), nz);
        if (!isfinite(nlen) || nlen < 1e-8)
            continue;
        nx /= nlen;
        ny /= nlen;
        nz /= nlen;
        double authored_ny = (double)m->vertices[i0].normal[1] + (double)m->vertices[i1].normal[1] +
                             (double)m->vertices[i2].normal[1];
        if (ny < 0.0 && isfinite(authored_ny) && authored_ny > 0.75) {
            nx = -nx;
            ny = -ny;
            nz = -nz;
        }

        nav_triangle_t *tri = &nm->source_triangles[nm->source_triangle_count++];
        tri->v[0] = (int32_t)i0;
        tri->v[1] = (int32_t)i1;
        tri->v[2] = (int32_t)i2;
        tri->normal[0] = (float)nx;
        tri->normal[1] = (float)ny;
        tri->normal[2] = (float)nz;
        tri->centroid[0] = (float)(((double)p0[0] + (double)p1[0] + (double)p2[0]) / 3.0);
        tri->centroid[1] = (float)(((double)p0[1] + (double)p1[1] + (double)p2[1]) / 3.0);
        tri->centroid[2] = (float)(((double)p0[2] + (double)p1[2] + (double)p2[2]) / 3.0);
        tri->neighbors[0] = tri->neighbors[1] = tri->neighbors[2] = -1;
        tri->blocked = 0;
        tri->area_id = 0;
        tri->traversal_cost = 1.0f;
    }

    if (nm->source_triangle_count == 0) {
        navmesh3d_free_partial(nm);
        return NULL;
    }

    if (!navmesh3d_apply_slope_filter(nm)) {
        navmesh3d_free_partial(nm);
        rt_trap("NavMesh3D: adjacency build failed (non-manifold edge or allocation failure)");
        return NULL;
    }

    return nm;
}

/// @brief Recompute a navmesh triangle's cached geometry (centroid and normal) from its vertices.
/// @param nm Borrowed navmesh supplying vertices.
/// @param t Mutable triangle whose cached centroid and normal are replaced.
/// @return 1 when indices, positions, and triangle area are valid, otherwise 0.
static int navmesh3d_recompute_triangle_geometry(rt_navmesh3d *nm, nav_triangle_t *t) {
    if (!nm || !t || !nm->vertices)
        return 0;
    for (int k = 0; k < 3; k++) {
        if (t->v[k] < 0 || t->v[k] >= nm->vertex_count)
            return 0;
    }
    const float *p0 = nm->vertices[t->v[0]].position;
    const float *p1 = nm->vertices[t->v[1]].position;
    const float *p2 = nm->vertices[t->v[2]].position;
    if (!navmesh3d_float3_finite(p0) || !navmesh3d_float3_finite(p1) ||
        !navmesh3d_float3_finite(p2))
        return 0;
    t->centroid[0] = (float)(((double)p0[0] + (double)p1[0] + (double)p2[0]) / 3.0);
    t->centroid[1] = (float)(((double)p0[1] + (double)p1[1] + (double)p2[1]) / 3.0);
    t->centroid[2] = (float)(((double)p0[2] + (double)p1[2] + (double)p2[2]) / 3.0);
    double ux = (double)p1[0] - (double)p0[0];
    double uy = (double)p1[1] - (double)p0[1];
    double uz = (double)p1[2] - (double)p0[2];
    double wx = (double)p2[0] - (double)p0[0];
    double wy = (double)p2[1] - (double)p0[1];
    double wz = (double)p2[2] - (double)p0[2];
    double nx = uy * wz - uz * wy;
    double ny = uz * wx - ux * wz;
    double nz = ux * wy - uy * wx;
    double len = hypot(hypot(nx, ny), nz);
    if (!isfinite(len) || len <= 1e-8)
        return 0;
    t->normal[0] = (float)(nx / len);
    t->normal[1] = (float)(ny / len);
    t->normal[2] = (float)(nz / len);
    return 1;
}

/// @brief Serialize one navmesh triangle (vertex indices, blocked flag, area id, sanitized
///   traversal cost) to @p f.
/// @param f Open destination stream.
/// @param t Borrowed triangle record.
/// @return Non-zero when the complete record is written.
static int navmesh3d_write_triangle_record(FILE *f, const nav_triangle_t *t) {
    return t && navmesh3d_write_i32(f, t->v[0]) && navmesh3d_write_i32(f, t->v[1]) &&
           navmesh3d_write_i32(f, t->v[2]) && navmesh3d_write_u8(f, t->blocked ? 1u : 0u) &&
           navmesh3d_write_i32(f, t->area_id) &&
           navmesh3d_write_f32(f, navmesh3d_sanitize_traversal_cost((double)t->traversal_cost));
}

/// @brief Validate the serialized fields of one triangle against the current vertex/area tables.
/// @param nm Borrowed navmesh owning the referenced tables.
/// @param t Borrowed triangle to validate.
/// @param area_name_count Number of valid area-name entries.
/// @return Non-zero when every serialized index and scalar is valid.
static int navmesh3d_triangle_record_export_valid(const rt_navmesh3d *nm,
                                                  const nav_triangle_t *t,
                                                  int32_t area_name_count) {
    if (!nm || !t || t->area_id < 0 || t->area_id > area_name_count)
        return 0;
    for (int lane = 0; lane < 3; lane++) {
        if (t->v[lane] < 0 || t->v[lane] >= nm->vertex_count)
            return 0;
    }
    return isfinite(t->traversal_cost);
}

/// @brief Deserialize one navmesh triangle from @p f, validating its vertex and area indices
///   against the loaded vertex count and @p area_name_count.
/// @param f Open source stream.
/// @param nm Borrowed partially imported navmesh supplying the vertex table.
/// @param t Caller-owned triangle record to initialize.
/// @param area_name_count Number of valid imported area names.
/// @return 1 when the record and derived geometry are valid, otherwise 0.
static int navmesh3d_read_triangle_record(FILE *f,
                                          rt_navmesh3d *nm,
                                          nav_triangle_t *t,
                                          int32_t area_name_count) {
    uint8_t blocked = 0;
    if (!t || !navmesh3d_read_i32(f, &t->v[0]) || !navmesh3d_read_i32(f, &t->v[1]) ||
        !navmesh3d_read_i32(f, &t->v[2]) || !navmesh3d_read_u8(f, &blocked) ||
        !navmesh3d_read_i32(f, &t->area_id) || !navmesh3d_read_f32(f, &t->traversal_cost))
        return 0;
    if (t->area_id < 0 || t->area_id > area_name_count)
        return 0;
    t->neighbors[0] = t->neighbors[1] = t->neighbors[2] = -1;
    t->blocked = blocked ? 1 : 0;
    t->traversal_cost = navmesh3d_sanitize_traversal_cost((double)t->traversal_cost);
    return navmesh3d_recompute_triangle_geometry(nm, t);
}

/// @brief Write a complete navmesh to @p f in the versioned VNAVMSH2 binary format (header
///   params, vertices, source + baked triangles, off-mesh links, obstacles, and area names).
/// @param nm Borrowed complete navmesh to serialize.
/// @param f Open destination stream.
/// @return Non-zero when every record is written successfully.
static int navmesh3d_export_v2(rt_navmesh3d *nm, FILE *f) {
    const char magic[8] = {'V', 'N', 'A', 'V', 'M', 'S', 'H', '2'};
    int32_t vc = nm->vertex_count < 0 ? 0 : nm->vertex_count;
    int32_t source_tc =
        (nm->source_triangles && nm->source_triangle_count > 0) ? nm->source_triangle_count : 0;
    int32_t tc = navmesh3d_read_triangle_count(nm);
    int32_t area_count = navmesh3d_read_area_name_count(nm);
    int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
    int32_t obstacle_count = navmesh3d_read_obstacle_count(nm);
    if ((vc > 0 && !nm->vertices) || (source_tc > 0 && !nm->source_triangles) ||
        (tc > 0 && !nm->triangles) || (area_count > 0 && !nm->area_names) ||
        (link_count > 0 && !nm->offmesh_links) || (obstacle_count > 0 && !nm->obstacles))
        return 0;
    for (int32_t i = 0; i < vc; i++) {
        if (!navmesh3d_float3_finite(nm->vertices[i].position))
            return 0;
    }
    for (int32_t i = 0; i < area_count; i++) {
        if (!nm->area_names[i] || !rt_string_is_handle(nm->area_names[i]) ||
            rt_str_len(nm->area_names[i]) <= 0)
            return 0;
    }
    for (int32_t i = 0; i < source_tc; i++) {
        if (!navmesh3d_triangle_record_export_valid(nm, &nm->source_triangles[i], area_count))
            return 0;
    }
    for (int32_t i = 0; i < tc; i++) {
        if (!navmesh3d_triangle_record_export_valid(nm, &nm->triangles[i], area_count))
            return 0;
    }
    int ok = navmesh3d_io_write(f, magic, sizeof(magic)) &&
             navmesh3d_write_f64(f, navmesh3d_agent_dim_or(nm->agent_radius, 0.4)) &&
             navmesh3d_write_f64(f, navmesh3d_agent_dim_or(nm->agent_height, 1.8)) &&
             navmesh3d_write_f64(f, navmesh3d_sanitize_slope(nm->max_slope)) &&
             navmesh3d_write_f64(f,
                                 isfinite(nm->tile_size) && nm->tile_size > 0.0 &&
                                         nm->tile_size <= NAVMESH3D_TILE_SIZE_MAX
                                     ? nm->tile_size
                                     : 0.0) &&
             navmesh3d_write_u8(f, nm->skip_portal_width_gate ? 1u : 0u) &&
             navmesh3d_write_i32(f, vc);
    for (int32_t i = 0; ok && i < vc; i++) {
        ok = navmesh3d_write_f32(f, nm->vertices[i].position[0]) &&
             navmesh3d_write_f32(f, nm->vertices[i].position[1]) &&
             navmesh3d_write_f32(f, nm->vertices[i].position[2]);
    }
    ok = ok && navmesh3d_write_i32(f, area_count);
    for (int32_t i = 0; ok && i < area_count; i++)
        ok = navmesh3d_write_string(f, nm->area_names ? nm->area_names[i] : NULL);
    ok = ok && navmesh3d_write_i32(f, source_tc);
    for (int32_t i = 0; ok && i < source_tc; i++)
        ok = navmesh3d_write_triangle_record(f, &nm->source_triangles[i]);
    ok = ok && navmesh3d_write_i32(f, tc);
    for (int32_t i = 0; ok && i < tc; i++)
        ok = navmesh3d_write_triangle_record(f, &nm->triangles[i]);
    ok = ok && navmesh3d_write_i32(f, link_count);
    for (int32_t i = 0; ok && i < link_count; i++) {
        const nav_offmesh_link_t *link = &nm->offmesh_links[i];
        if (!navmesh3d_float3_finite(link->from) || !navmesh3d_float3_finite(link->to) ||
            (link->kind && !rt_string_is_handle(link->kind))) {
            ok = 0;
            break;
        }
        ok = navmesh3d_write_f32(f, link->from[0]) && navmesh3d_write_f32(f, link->from[1]) &&
             navmesh3d_write_f32(f, link->from[2]) && navmesh3d_write_f32(f, link->to[0]) &&
             navmesh3d_write_f32(f, link->to[1]) && navmesh3d_write_f32(f, link->to[2]) &&
             navmesh3d_write_u8(f, link->bidirectional ? 1u : 0u) &&
             navmesh3d_write_i64(f, link->state_flags) &&
             navmesh3d_write_f32(f,
                                 navmesh3d_sanitize_traversal_cost((double)link->traversal_cost)) &&
             navmesh3d_write_string(f, link->kind);
    }
    ok = ok && navmesh3d_write_i32(f, obstacle_count);
    for (int32_t i = 0; ok && i < obstacle_count; i++) {
        const nav_obstacle_t *obstacle = &nm->obstacles[i];
        if (!navmesh3d_float3_finite(obstacle->min) || !navmesh3d_float3_finite(obstacle->max) ||
            obstacle->min[0] > obstacle->max[0] || obstacle->min[1] > obstacle->max[1] ||
            obstacle->min[2] > obstacle->max[2]) {
            ok = 0;
            break;
        }
        ok = navmesh3d_write_f32(f, obstacle->min[0]) && navmesh3d_write_f32(f, obstacle->min[1]) &&
             navmesh3d_write_f32(f, obstacle->min[2]) && navmesh3d_write_f32(f, obstacle->max[0]) &&
             navmesh3d_write_f32(f, obstacle->max[1]) && navmesh3d_write_f32(f, obstacle->max[2]);
    }
    return ok;
}

/// @brief Serialize a baked navmesh to a versioned binary file ("VNAVMSH2").
/// @details v2 records explicit little-endian geometry, traversal costs, blocked state, area
///   labels, off-mesh links, obstacles, source triangles for future area/obstacle edits, and agent
///   params. Runtime-derived adjacency/query grids are rebuilt on import; retained voxel
///   heightfield source data is not serialized.
/// @param obj Opaque NavMesh3D handle.
/// @param path Runtime string naming the destination file.
/// @return 1 after atomic temporary-file replacement succeeds, otherwise 0.
int8_t rt_navmesh3d_export(void *obj, rt_string path) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm || !path || !rt_string_is_handle(path))
        return 0;
    const char *cpath = rt_string_cstr(path);
    if (!cpath || cpath[0] == '\0')
        return 0;
    char *tmp_path = NULL;
    FILE *f = rt_file_stdio_open_temp_for_replace_utf8(cpath, &tmp_path);
    if (!f)
        return 0;
    int ok = navmesh3d_export_v2(nm, f);
    if (fclose(f) != 0)
        ok = 0;
    if (ok)
        ok = rt_file_stdio_replace_utf8(tmp_path, cpath) ? 1 : 0;
    if (!ok && tmp_path)
        (void)rt_file_stdio_unlink_utf8(tmp_path);
    free(tmp_path);
    return ok ? 1 : 0;
}

/// @brief Parse a legacy v1 navmesh body (after its magic has been consumed) into a fresh
///   NavMesh3D, or NULL on malformed or oversized data.
/// @param f Open binary stream positioned immediately after the eight-byte version-one magic.
/// @return New GC-managed NavMesh3D with rebuilt derived indexes, or null after releasing partial
///         state on malformed input.
static rt_navmesh3d *navmesh3d_import_v1_after_magic(FILE *f) {
    double params[3];
    int32_t vc = 0;
    if (!navmesh3d_read_f64(f, &params[0]) || !navmesh3d_read_f64(f, &params[1]) ||
        !navmesh3d_read_f64(f, &params[2]) || !navmesh3d_read_i32(f, &vc) || vc < 0 ||
        vc > NAVMESH3D_IMPORT_MAX_RECORDS)
        return NULL;

    rt_navmesh3d *nm =
        (rt_navmesh3d *)rt_obj_new_i64(RT_G3D_NAVMESH3D_CLASS_ID, (int64_t)sizeof(rt_navmesh3d));
    if (!nm) {
        rt_trap("NavMesh3D.Import: allocation failed");
        return NULL;
    }
    memset(nm, 0, sizeof(*nm));
    rt_obj_set_finalizer(nm, navmesh3d_finalizer);
    if (!navmesh3d_init_path_workspace_gate(nm))
        goto fail;
    nm->agent_radius = navmesh3d_agent_dim_or(params[0], 0.4);
    nm->agent_height = navmesh3d_agent_dim_or(params[1], 1.8);
    nm->max_slope = navmesh3d_sanitize_slope(params[2]);

    if (vc > 0) {
        nm->vertices = (nav_vertex_t *)malloc((size_t)vc * sizeof(nav_vertex_t));
        if (!nm->vertices)
            goto fail;
        for (int32_t i = 0; i < vc; i++) {
            if (!navmesh3d_read_f32(f, &nm->vertices[i].position[0]) ||
                !navmesh3d_read_f32(f, &nm->vertices[i].position[1]) ||
                !navmesh3d_read_f32(f, &nm->vertices[i].position[2]))
                goto fail;
            nm->vertices[i].position[0] =
                navmesh3d_coordf_or((double)nm->vertices[i].position[0], 0.0);
            nm->vertices[i].position[1] =
                navmesh3d_coordf_or((double)nm->vertices[i].position[1], 0.0);
            nm->vertices[i].position[2] =
                navmesh3d_coordf_or((double)nm->vertices[i].position[2], 0.0);
        }
    }
    nm->vertex_count = vc;

    int32_t tc = 0;
    if (!navmesh3d_read_i32(f, &tc) || tc < 0 || tc > NAVMESH3D_IMPORT_MAX_RECORDS)
        goto fail;
    if (tc > 0) {
        nm->triangles = (nav_triangle_t *)calloc((size_t)tc, sizeof(nav_triangle_t));
        if (!nm->triangles)
            goto fail;
        for (int32_t i = 0; i < tc; i++) {
            nav_triangle_t *t = &nm->triangles[i];
            int32_t idx[3];
            float cost = 1.0f;
            int8_t blocked = 0;
            if (!navmesh3d_read_i32(f, &idx[0]) || !navmesh3d_read_i32(f, &idx[1]) ||
                !navmesh3d_read_i32(f, &idx[2]) || !navmesh3d_read_f32(f, &cost) ||
                !navmesh3d_io_read(f, &blocked, sizeof(blocked)))
                goto fail;
            for (int k = 0; k < 3; k++) {
                if (idx[k] < 0 || idx[k] >= vc)
                    goto fail;
                t->v[k] = idx[k];
            }
            t->neighbors[0] = t->neighbors[1] = t->neighbors[2] = -1;
            t->area_id = 0;
            t->traversal_cost = navmesh3d_sanitize_traversal_cost((double)cost);
            t->blocked = blocked ? 1 : 0;
            if (!navmesh3d_recompute_triangle_geometry(nm, t))
                goto fail;
            nm->triangle_count = i + 1;
        }
    }
    if (fgetc(f) != EOF)
        goto fail;

    if (tc > 0) {
        nm->source_triangles = (nav_triangle_t *)malloc((size_t)tc * sizeof(nav_triangle_t));
        if (!nm->source_triangles)
            goto fail;
        memcpy(nm->source_triangles, nm->triangles, (size_t)tc * sizeof(nav_triangle_t));
        nm->source_triangle_count = tc;
    }

    if (!navmesh3d_build_adjacency(nm))
        goto fail;
    (void)navmesh3d_build_query_grid(nm);
    nm->topology_revision = 1u;
    return nm;

fail:
    navmesh3d_free_partial(nm);
    return NULL;
}
