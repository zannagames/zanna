//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/nav/rt_navmesh3d_query_grid_stage.inc
// Purpose: Transactional construction of NavMesh3D's bounded spatial query-grid candidate.
//
// Key invariants:
//   - Live query-grid arrays are never mutated during candidate construction.
//   - Cell/reference products are checked before allocation and indexing.
//   - Bounded resource refusal is distinct from allocation/structural failure.
//
// Ownership/Lifetime:
//   - Included by rt_navmesh3d_build.inc after query-grid stage types and range helpers.
//   - Successful output arrays transfer to the caller-owned stage; every failure frees scratch.
//
// Links: rt_navmesh3d_build.inc, rt_navmesh3d_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_navmesh3d_query_grid_stage.inc
 * @brief Builds transactional candidates for NavMesh3D's bounded spatial query grid.
 *
 * The staging routine replicates triangle references into a checked uniform
 * XZ grid without mutating the live index. Successful allocations transfer to
 * the caller's stage object; resource-budget refusal requests a bounded linear
 * fallback, while structural or allocation failures remain distinguishable so
 * callers can preserve previously published state.
 */

/// @brief Construct a complete candidate uniform XZ query grid without touching live state.
/// @details Each triangle is replicated into every cell overlapped by its XZ AABB. Cell and
/// reference products use checked `uint64_t`/`size_t` arithmetic. If configured budgets would be
/// exceeded, an all-zero bounded fallback is returned; allocation failure remains distinct so a
/// rebuild can retain its prior index bit-for-bit.
/// @param nm Navigation mesh supplying vertices.
/// @param triangles Candidate triangle set to index.
/// @param triangle_count Number of candidate triangles.
/// @param stage Zeroed output stage receiving owned arrays only on success.
/// @return READY for a complete grid, BOUNDED_FALLBACK for a safe linear scan, or ERROR for
/// invalid input, injected failure, or allocation failure.
static navmesh3d_query_grid_result navmesh3d_stage_query_grid(const rt_navmesh3d *nm,
                                                              const nav_triangle_t *triangles,
                                                              int32_t triangle_count,
                                                              navmesh3d_query_grid_stage *stage) {
    double min_x = DBL_MAX, min_z = DBL_MAX, max_x = -DBL_MAX, max_z = -DBL_MAX;
    double span_x, span_z, cell, inv;
    int32_t dim, gnx, gnz;
    uint64_t ncells64;
    uint64_t total64 = 0;
    size_t ncells;
    int32_t *starts = NULL;
    int32_t *tris = NULL;
    int32_t *cursor = NULL;
    if (!stage)
        return NAVMESH3D_QUERY_GRID_ERROR;
    memset(stage, 0, sizeof(*stage));
    if (!nm || triangle_count < 0 || (triangle_count > 0 && (!triangles || !nm->vertices)))
        return NAVMESH3D_QUERY_GRID_ERROR;
    if (triangle_count == 0)
        return NAVMESH3D_QUERY_GRID_READY;
    for (int32_t i = 0; i < triangle_count; i++) {
        const nav_triangle_t *t = &triangles[i];
        for (int k = 0; k < 3; k++) {
            if (t->v[k] < 0 || t->v[k] >= nm->vertex_count)
                return NAVMESH3D_QUERY_GRID_ERROR;
            const float *p = nm->vertices[t->v[k]].position;
            if (!navmesh3d_float3_finite(p))
                return NAVMESH3D_QUERY_GRID_ERROR;
            if (p[0] < min_x)
                min_x = p[0];
            if (p[0] > max_x)
                max_x = p[0];
            if (p[2] < min_z)
                min_z = p[2];
            if (p[2] > max_z)
                max_z = p[2];
        }
    }
    if (!(max_x >= min_x) || !(max_z >= min_z) || !isfinite(min_x) || !isfinite(min_z) ||
        !isfinite(max_x) || !isfinite(max_z))
        return NAVMESH3D_QUERY_GRID_BOUNDED_FALLBACK;
    span_x = max_x - min_x;
    span_z = max_z - min_z;
    if (!isfinite(span_x) || !isfinite(span_z) || span_x < 0.0 || span_z < 0.0)
        return NAVMESH3D_QUERY_GRID_BOUNDED_FALLBACK;
    dim = (int32_t)sqrt((double)triangle_count);
    if (dim < 1)
        dim = 1;
    if (dim > 256)
        dim = 256;
    cell = fmax(span_x, span_z) / (double)dim;
    if (!(cell > 1e-9))
        cell = 1.0;
    inv = 1.0 / cell;
    if (!isfinite(inv) || inv <= 0.0)
        return NAVMESH3D_QUERY_GRID_BOUNDED_FALLBACK;
    double gnx_value = floor(span_x * inv) + 1.0;
    double gnz_value = floor(span_z * inv) + 1.0;
    if (!isfinite(gnx_value) || !isfinite(gnz_value) || gnx_value < 1.0 || gnz_value < 1.0)
        return NAVMESH3D_QUERY_GRID_BOUNDED_FALLBACK;
    if (gnx_value > (double)NAVMESH3D_QUERY_GRID_MAX_DIM)
        gnx_value = (double)NAVMESH3D_QUERY_GRID_MAX_DIM;
    if (gnz_value > (double)NAVMESH3D_QUERY_GRID_MAX_DIM)
        gnz_value = (double)NAVMESH3D_QUERY_GRID_MAX_DIM;
    gnx = (int32_t)gnx_value;
    gnz = (int32_t)gnz_value;
    ncells64 = (uint64_t)(uint32_t)gnx * (uint64_t)(uint32_t)gnz;
    if (ncells64 == 0 || ncells64 > NAVMESH3D_QUERY_GRID_MAX_CELLS ||
        ncells64 > (uint64_t)SIZE_MAX || ncells64 + 1u > (uint64_t)(SIZE_MAX / sizeof(int32_t)))
        return NAVMESH3D_QUERY_GRID_BOUNDED_FALLBACK;
    ncells = (size_t)ncells64;
    if (g_navmesh3d_test_force_query_grid_alloc_failure)
        return NAVMESH3D_QUERY_GRID_ERROR;
    starts = (int32_t *)calloc(ncells + 1, sizeof(int32_t));
    if (!starts)
        return NAVMESH3D_QUERY_GRID_ERROR;
    for (int32_t i = 0; i < triangle_count; i++) {
        int32_t cx0, cx1, cz0, cz1;
        if (!navmesh3d_tri_cell_range(
                nm, &triangles[i], min_x, min_z, inv, gnx, gnz, &cx0, &cx1, &cz0, &cz1))
            goto query_grid_error;
        uint64_t width = (uint64_t)(uint32_t)(cx1 - cx0 + 1);
        uint64_t depth = (uint64_t)(uint32_t)(cz1 - cz0 + 1);
        if (depth != 0 && width > UINT64_MAX / depth)
            goto query_grid_fallback;
        uint64_t replicated = width * depth;
        if (replicated > NAVMESH3D_QUERY_GRID_MAX_REFERENCES - total64)
            goto query_grid_fallback;
        total64 += replicated;
        for (int32_t cz = cz0; cz <= cz1; cz++) {
            for (int32_t cx = cx0; cx <= cx1; cx++) {
                size_t slot = (size_t)cz * (size_t)gnx + (size_t)cx + 1u;
                if (starts[slot] == INT32_MAX)
                    goto query_grid_fallback;
                starts[slot]++;
            }
        }
    }
    uint64_t prefix = 0;
    for (size_t c = 0; c < ncells; c++) {
        prefix += (uint64_t)(uint32_t)starts[c + 1];
        if (prefix > NAVMESH3D_QUERY_GRID_MAX_REFERENCES || prefix > INT32_MAX)
            goto query_grid_fallback;
        starts[c + 1] = (int32_t)prefix;
    }
    if (prefix != total64 || total64 > (uint64_t)(SIZE_MAX / sizeof(int32_t)))
        goto query_grid_fallback;
    tris = (int32_t *)malloc((size_t)(total64 ? total64 : 1u) * sizeof(int32_t));
    cursor = (int32_t *)malloc(ncells * sizeof(int32_t));
    if (!tris || !cursor)
        goto query_grid_error;
    for (size_t c = 0; c < ncells; c++)
        cursor[c] = starts[c];
    for (int32_t i = 0; i < triangle_count; i++) {
        int32_t cx0, cx1, cz0, cz1;
        if (!navmesh3d_tri_cell_range(
                nm, &triangles[i], min_x, min_z, inv, gnx, gnz, &cx0, &cx1, &cz0, &cz1))
            goto query_grid_error;
        for (int32_t cz = cz0; cz <= cz1; cz++)
            for (int32_t cx = cx0; cx <= cx1; cx++)
                tris[cursor[(size_t)cz * (size_t)gnx + (size_t)cx]++] = i;
    }
    free(cursor);
    stage->min_x = min_x;
    stage->min_z = min_z;
    stage->inv_cell = inv;
    stage->nx = gnx;
    stage->nz = gnz;
    stage->starts = starts;
    stage->tris = tris;
    return NAVMESH3D_QUERY_GRID_READY;

query_grid_fallback:
    free(starts);
    free(tris);
    free(cursor);
    return NAVMESH3D_QUERY_GRID_BOUNDED_FALLBACK;

query_grid_error:
    free(starts);
    free(tris);
    free(cursor);
    return NAVMESH3D_QUERY_GRID_ERROR;
}
