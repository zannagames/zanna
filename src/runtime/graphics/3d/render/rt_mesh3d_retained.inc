//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_mesh3d_retained.inc
// Purpose: Immutable Mesh3D geometry-revision ownership and tangent-variant caching.
//
// Key invariants:
//   - A published revision never aliases mutable Mesh3D authoring arrays.
//   - Reference counts cover both mesh ownership and every queued Canvas3D command.
//   - Allocation failure cannot publish a partially initialized revision or tangent variant.
//
// Ownership/Lifetime:
//   - Included by rt_mesh3d.c after its validation/counter helpers.
//   - Each revision exclusively owns its copied vertices, indices, and optional tangent vertices.
//
// Links: rt_mesh3d.c, rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements immutable Mesh3D geometry revisions and tangent-variant caching.
/// @details This include fragment is compiled as part of `rt_mesh3d.c`. Retained revisions own
///          transactional copies of authored geometry so queued Canvas3D commands remain stable
///          across later mesh mutations, with atomic references spanning mesh and frame owners.

/// @brief Acquire one reference to an immutable retained geometry revision.
/// @details The process-wide renderer can hand revisions between a mesh and one or more canvases;
/// an atomic count keeps destruction well-defined even when frame cleanup occurs on a different
/// runtime thread from the original publication.
/// @param revision Revision to retain, or NULL for a no-op.
void rt_mesh3d_geometry_revision_retain(rt_mesh3d_geometry_revision *revision) {
    if (revision)
        (void)rt_atomic_fetch_add_i32(&revision->ref_count, 1, __ATOMIC_ACQ_REL);
}

/// @brief Release one immutable retained-geometry reference and free the last-owned payload.
/// @details The raw vertices, indices, and optional generated-tangent variant are all owned by the
/// revision object and are destroyed together after the final mesh/canvas reference drops.
/// @param revision Revision to release, or NULL for a no-op.
void rt_mesh3d_geometry_revision_release(rt_mesh3d_geometry_revision *revision) {
    if (!revision)
        return;
    if (rt_atomic_fetch_sub_i32(&revision->ref_count, 1, __ATOMIC_ACQ_REL) != 1)
        return;
    free(revision->vertices);
    revision->vertices = NULL;
    free(revision->indices);
    revision->indices = NULL;
    free(revision->tangent_vertices);
    revision->tangent_vertices = NULL;
    free(revision);
}

/// @brief Detach a mesh from its currently published immutable geometry revision.
/// @details A canvas may still own the same revision, so detaching implements revision forking:
/// mutation never edits storage referenced by commands that were queued earlier.
/// @param mesh Mesh whose owner reference should be dropped.
void rt_mesh3d_invalidate_retained_geometry(rt_mesh3d *mesh) {
    rt_mesh3d_geometry_revision *revision;
    if (!mesh || !mesh->retained_geometry)
        return;
    revision = mesh->retained_geometry;
    mesh->retained_geometry = NULL;
    rt_mesh3d_geometry_revision_release(revision);
}

/// @brief Lazily publish an immutable copy of the mesh's current safe geometry ranges.
/// @details A matching revision/count tuple is returned without copying. On a miss, this routine
/// allocates exact-size vertex and index arrays, copies the source once, and installs one
/// mesh-owned reference. Allocation is transactional: no partially initialized revision is
/// published.
/// @param mesh Source Mesh3D payload.
/// @return Borrowed current revision, or NULL for empty geometry or allocation failure.
rt_mesh3d_geometry_revision *rt_mesh3d_get_retained_geometry(rt_mesh3d *mesh) {
    rt_mesh3d_geometry_revision *revision;
    uint32_t vertex_count;
    uint32_t index_count;
    size_t vertex_bytes;
    size_t index_bytes;
    if (!mesh)
        return NULL;
    rt_mesh3d_repair_geometry_counts(mesh);
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    index_count = rt_mesh3d_safe_index_count(mesh);
    if (!mesh->vertices || !mesh->indices || vertex_count == 0 || index_count == 0)
        return NULL;
    if (mesh->retained_geometry &&
        mesh->retained_geometry->source_revision == mesh->geometry_revision &&
        mesh->retained_geometry->vertex_count == vertex_count &&
        mesh->retained_geometry->index_count == index_count) {
        mesh3d_increment_counter(&mesh->retained_geometry_cache_hit_count);
        return mesh->retained_geometry;
    }
    rt_mesh3d_invalidate_retained_geometry(mesh);
    if ((size_t)vertex_count > SIZE_MAX / sizeof(vgfx3d_vertex_t) ||
        (size_t)index_count > SIZE_MAX / sizeof(uint32_t))
        return NULL;
    vertex_bytes = (size_t)vertex_count * sizeof(vgfx3d_vertex_t);
    index_bytes = (size_t)index_count * sizeof(uint32_t);
    revision = (rt_mesh3d_geometry_revision *)calloc(1, sizeof(*revision));
    if (!revision)
        return NULL;
    revision->vertices = (vgfx3d_vertex_t *)malloc(vertex_bytes);
    revision->indices = (uint32_t *)malloc(index_bytes);
    if (!revision->vertices || !revision->indices) {
        free(revision->vertices);
        free(revision->indices);
        free(revision);
        return NULL;
    }
    memcpy(revision->vertices, mesh->vertices, vertex_bytes);
    memcpy(revision->indices, mesh->indices, index_bytes);
    revision->ref_count = 1;
    revision->source_revision = mesh->geometry_revision ? mesh->geometry_revision : 1u;
    revision->vertex_count = vertex_count;
    revision->index_count = index_count;
    revision->tangent_vertices = NULL;
    revision->tangent_state = 0;
    mesh->retained_geometry = revision;
    mesh3d_increment_counter(&mesh->retained_geometry_build_count);
    return revision;
}

/// @brief Build or reuse the immutable generated-tangent variant of a retained revision.
/// @details The raw revision bytes are copied once into a sibling vertex array, then the existing
/// Mesh3D tangent generator runs against a stack facade. Source vertices are never modified. A
/// completed variant is reusable because all inputs belong to the immutable source revision.
/// @param mesh Mesh that owns diagnostic build/hit counters.
/// @param revision Immutable geometry revision to augment.
/// @return Non-zero when a tangent-bearing vertex array is ready, otherwise zero.
int rt_mesh3d_geometry_revision_ensure_tangents(rt_mesh3d *mesh,
                                                rt_mesh3d_geometry_revision *revision) {
    rt_mesh3d temp;
    size_t vertex_bytes;
    if (!mesh || !revision || !revision->vertices || !revision->indices ||
        revision->vertex_count == 0 || revision->index_count == 0)
        return 0;
    if (revision->tangent_state > 0 && revision->tangent_vertices) {
        mesh3d_increment_counter(&mesh->retained_tangent_cache_hit_count);
        return 1;
    }
    if ((size_t)revision->vertex_count > SIZE_MAX / sizeof(vgfx3d_vertex_t))
        return 0;
    vertex_bytes = (size_t)revision->vertex_count * sizeof(vgfx3d_vertex_t);
    revision->tangent_vertices = (vgfx3d_vertex_t *)malloc(vertex_bytes);
    if (!revision->tangent_vertices)
        return 0;
    memcpy(revision->tangent_vertices, revision->vertices, vertex_bytes);
    memset(&temp, 0, sizeof(temp));
    temp.vertices = revision->tangent_vertices;
    temp.vertex_count = revision->vertex_count;
    temp.vertex_capacity = revision->vertex_count;
    temp.indices = revision->indices;
    temp.index_count = revision->index_count;
    temp.index_capacity = revision->index_count;
    temp.geometry_revision = revision->source_revision;
    temp.transient_geometry_facade = 1;
    rt_mesh3d_calc_tangents_impl(&temp);
    if (!temp.tangents_ready) {
        free(revision->tangent_vertices);
        revision->tangent_vertices = NULL;
        revision->tangent_state = 0;
        return 0;
    }
    revision->tangent_state = 1;
    mesh3d_increment_counter(&mesh->retained_tangent_build_count);
    return 1;
}
