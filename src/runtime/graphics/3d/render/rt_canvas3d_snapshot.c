//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_snapshot.c
// Purpose: Canvas3D deferred mesh geometry binding. Heap meshes retain immutable
//   cross-frame geometry revisions; stack/rebased geometry uses canvas-owned
//   frame snapshots. Missing tangent variants are cached with retained revisions.
// Key invariants:
//   - A queued heap draw holds a revision reference until frame cleanup, so a
//     later source mutation cannot invalidate command vertex/index pointers.
//   - Cache hits require an exact source, revision, vertex-count, and index-count
//     tuple; generated tangents never mutate the raw retained vertex variant.
// Ownership/Lifetime:
//   - Mesh3D owns the current retained revision; snapshot-table entries acquire
//     one frame reference and release it before the table count resets.
//   - Rebased and transient copies remain owned by the frame temp-buffer tracker.
// Links: rt_canvas3d_internal.h, rt_canvas3d_tempmgr.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements stable deferred geometry bindings for Canvas3D mesh draws.
/// @details The module hashes frame-local bindings, retains immutable heap-mesh
///   revisions, snapshots transient geometry within a byte budget, supports
///   camera-relative rebasing, and caches generated tangent variants.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_heap.h"

#include <stdlib.h>
#include <string.h>

/// @brief Build the revision/count key used by the per-frame mesh snapshot hash table.
/// @details The hash table indexes immutable snapshot entries by the source mesh handle plus the
///   geometry revision and validated vertex/index counts. Tangent state is deliberately excluded:
///   an existing plain snapshot can be upgraded in place by tangent generation.
/// @param source Non-null source identity; its pointer value is hashed but not dereferenced.
/// @param geometry_revision Source geometry revision represented by the binding.
/// @param vertex_count Validated vertex count represented by the binding.
/// @param index_count Validated index count represented by the binding.
/// @return Deterministic nonzero hash key for the supplied tuple.
static uint64_t canvas3d_mesh_snapshot_key(void *source,
                                           uint32_t geometry_revision,
                                           uint32_t vertex_count,
                                           uint32_t index_count) {
    uint64_t key = canvas3d_hash_u64((uint64_t)(uintptr_t)source);
    key = canvas3d_hash_u64(key ^ ((uint64_t)geometry_revision << 32u));
    key = canvas3d_hash_u64(key ^ ((uint64_t)vertex_count << 16u) ^ (uint64_t)index_count);
    return key ? key : 1u;
}

/// @brief Return whether one snapshot entry matches the requested source/revision/count tuple.
/// @param entry Borrowed snapshot entry to inspect; may be `NULL`.
/// @param source Exact source identity required.
/// @param geometry_revision Exact source revision required.
/// @param vertex_count Exact vertex count required.
/// @param index_count Exact index count required.
/// @return Nonzero only when @p entry exists and every key field matches.
static int canvas3d_mesh_snapshot_entry_matches(const rt_canvas3d_mesh_snapshot_entry *entry,
                                                void *source,
                                                uint32_t geometry_revision,
                                                uint32_t vertex_count,
                                                uint32_t index_count) {
    return entry && entry->source == source && entry->geometry_revision == geometry_revision &&
           entry->vertex_count == vertex_count && entry->index_count == index_count;
}

/// @brief Clear the snapshot hash table to all-empty slots.
/// @param c Canvas whose allocated hash slots are reset to the empty sentinel;
///   null or unavailable tables are ignored.
void canvas3d_mesh_snapshot_hash_clear(rt_canvas3d *c) {
    if (!c || !c->mesh_snapshot_hash || c->mesh_snapshot_hash_capacity <= 0)
        return;
    for (int32_t i = 0; i < c->mesh_snapshot_hash_capacity; ++i)
        c->mesh_snapshot_hash[i] = -1;
}

/// @brief Insert an existing snapshot entry index into the hash table.
/// @details Open addressing keeps the table allocation-free during lookups. Returns 0 only when
///   the table is unavailable or saturated; callers can still fall back to the linear snapshot
///   list.
/// @param c Canvas owning a power-of-two hash table and snapshot array.
/// @param entry_index Valid index in the current snapshot array to insert.
/// @return Nonzero when an empty probe slot accepted the index; zero for invalid
///   state, a source-less entry, or a saturated table.
static int canvas3d_mesh_snapshot_hash_insert(rt_canvas3d *c, int32_t entry_index) {
    rt_canvas3d_mesh_snapshot_entry *entry;
    uint64_t key;
    int32_t mask;
    int32_t slot;
    if (!c || !c->mesh_snapshot_hash || c->mesh_snapshot_hash_capacity <= 0 || entry_index < 0 ||
        entry_index >= c->mesh_snapshot_count)
        return 0;
    entry = &c->mesh_snapshots[entry_index];
    if (!entry->source)
        return 0;
    key = canvas3d_mesh_snapshot_key(
        entry->source, entry->geometry_revision, entry->vertex_count, entry->index_count);
    mask = c->mesh_snapshot_hash_capacity - 1;
    slot = (int32_t)(key & (uint32_t)mask);
    for (int32_t probe = 0; probe < c->mesh_snapshot_hash_capacity; ++probe) {
        if (c->mesh_snapshot_hash[slot] < 0) {
            c->mesh_snapshot_hash[slot] = entry_index;
            return 1;
        }
        slot = (slot + 1) & mask;
    }
    return 0;
}

/// @brief Ensure the snapshot hash table can index at least @p needed entries.
/// @details Rebuilds from the snapshot array after growth so lookup remains valid if the entry
///   array was reallocated. The hash capacity is always a power of two and at least twice the
///   requested entry count to keep probing short.
/// @param c Canvas owning the hash and snapshot arrays.
/// @param needed Nonnegative number of snapshot entries the table must index.
/// @return Nonzero when a complete rebuilt table is available. On overflow or
///   allocation failure, returns zero and marks the hash dirty so lookup scans remain valid.
static int canvas3d_ensure_mesh_snapshot_hash(rt_canvas3d *c, int32_t needed) {
    int32_t new_cap;
    int32_t *grown;
    if (!c || needed < 0)
        return 0;
    if (needed > (INT32_C(1) << 29)) {
        c->mesh_snapshot_hash_dirty = 1;
        return 0;
    }
    new_cap = canvas3d_next_power_of_two_i32(needed > 0 ? needed * 2 : 16);
    if (new_cap < 16)
        new_cap = 16;
    if (c->mesh_snapshot_hash_capacity != new_cap) {
        if ((size_t)new_cap > SIZE_MAX / sizeof(*c->mesh_snapshot_hash)) {
            c->mesh_snapshot_hash_dirty = 1;
            return 0;
        }
        grown = (int32_t *)realloc(c->mesh_snapshot_hash, (size_t)new_cap * sizeof(*grown));
        if (!grown) {
            c->mesh_snapshot_hash_dirty = 1;
            return 0;
        }
        c->mesh_snapshot_hash = grown;
        c->mesh_snapshot_hash_capacity = new_cap;
    }
    canvas3d_mesh_snapshot_hash_clear(c);
    for (int32_t i = 0; i < c->mesh_snapshot_count; ++i)
        (void)canvas3d_mesh_snapshot_hash_insert(c, i);
    c->mesh_snapshot_hash_dirty = 0;
    return 1;
}

/// @brief Find a cached snapshot entry index, using the hash table then falling back to a scan.
/// @param c Canvas containing the frame snapshot table.
/// @param source Non-null source identity to locate.
/// @param geometry_revision Required geometry revision.
/// @param vertex_count Required validated vertex count.
/// @param index_count Required validated index count.
/// @return Matching zero-based snapshot index, or `-1` when absent or input is invalid.
static int32_t canvas3d_find_mesh_snapshot(rt_canvas3d *c,
                                           void *source,
                                           uint32_t geometry_revision,
                                           uint32_t vertex_count,
                                           uint32_t index_count) {
    if (!c || !source)
        return -1;
    if (c->mesh_snapshot_hash && c->mesh_snapshot_hash_capacity > 0) {
        uint64_t key =
            canvas3d_mesh_snapshot_key(source, geometry_revision, vertex_count, index_count);
        int32_t mask = c->mesh_snapshot_hash_capacity - 1;
        int32_t slot = (int32_t)(key & (uint32_t)mask);
        for (int32_t probe = 0; probe < c->mesh_snapshot_hash_capacity; ++probe) {
            int32_t index = c->mesh_snapshot_hash[slot];
            if (index < 0)
                break;
            if (index < c->mesh_snapshot_count &&
                canvas3d_mesh_snapshot_entry_matches(&c->mesh_snapshots[index],
                                                     source,
                                                     geometry_revision,
                                                     vertex_count,
                                                     index_count))
                return index;
            slot = (slot + 1) & mask;
        }
        /* The probe terminated on an empty slot: a definitive miss. When the hash
         * indexes every snapshot (the common case) the linear scan below cannot find
         * anything more, so skip it. Only a failed (OOM) rebuild needs the fallback. */
        if (!c->mesh_snapshot_hash_dirty)
            return -1;
    }
    for (int32_t i = 0; i < c->mesh_snapshot_count; ++i) {
        if (canvas3d_mesh_snapshot_entry_matches(
                &c->mesh_snapshots[i], source, geometry_revision, vertex_count, index_count))
            return i;
    }
    return -1;
}

/// @brief Record a failed mesh-snapshot allocation/budget attempt for public diagnostics.
/// @details Snapshot failure is not always fatal: caller-owned stack meshes can fall back to
///   borrowed geometry, and missing generated tangents can degrade to a base normal-map-free draw.
///   The counters let applications detect those visual/performance fallbacks without changing the
///   legacy void draw API.
/// @param c Canvas that attempted the snapshot.
/// @param requested_bytes Total bytes the snapshot wanted to reserve.
static void canvas3d_record_mesh_snapshot_drop(rt_canvas3d *c, size_t requested_bytes) {
    if (!c)
        return;
    canvas3d_record_submission_failure(c, RT_CANVAS3D_SUBMISSION_SNAPSHOT_FAILURE);
    if (c->last_mesh_snapshot_drop_count < INT64_MAX)
        c->last_mesh_snapshot_drop_count++;
    if (requested_bytes > (size_t)(INT64_MAX - c->last_mesh_snapshot_dropped_bytes))
        c->last_mesh_snapshot_dropped_bytes = INT64_MAX;
    else
        c->last_mesh_snapshot_dropped_bytes += (int64_t)requested_bytes;
}

/// @brief Consume the one-shot CTest snapshot failure before publishing snapshot state.
/// @details Production callers pass the exact overflow-checked byte request. When armed, this
///   helper clears the flag, updates both legacy snapshot-drop counters and additive submission
///   diagnostics, then asks the caller to return without allocating or changing cache entries.
/// @param c Canvas that may carry the one-shot test flag.
/// @param requested_bytes Bytes the rejected geometry binding would have required.
/// @return Non-zero only when an injected failure was consumed.
static int canvas3d_consume_injected_snapshot_failure(rt_canvas3d *c, size_t requested_bytes) {
    if (!c || !c->test_fail_next_mesh_snapshot)
        return 0;
    c->test_fail_next_mesh_snapshot = 0;
    canvas3d_record_mesh_snapshot_drop(c, requested_bytes);
    return 1;
}

/// @brief Copy mesh vertex+index arrays into canvas-owned temp buffers.
/// @details Used when the mesh's owning heap object may be freed before the GPU
///          consumes the draw command — the snapshot lives on the canvas's temp
///          buffer list, freed at end-of-frame. Returns 1 on success, 0 on
///          allocation failure or invalid mesh state.
/// @param c Canvas that tracks the resulting allocations and per-frame byte budget.
/// @param mesh Borrowed source mesh with nonempty validated vertex and index arrays.
/// @param out_vertices Output receiving the tracked vertex copy on success.
/// @param out_indices Output receiving the tracked index copy on success.
/// @return Nonzero on success; zero for invalid geometry, size overflow, budget or
///   injected rejection, allocation failure, or temp-tracker failure. Outputs are
///   not modified unless both buffers become tracked.
int canvas3d_snapshot_mesh_geometry(rt_canvas3d *c,
                                    const rt_mesh3d *mesh,
                                    vgfx3d_vertex_t **out_vertices,
                                    uint32_t **out_indices) {
    vgfx3d_vertex_t *vertices;
    uint32_t *indices;
    size_t vertex_bytes;
    size_t index_bytes;
    uint32_t vertex_count;
    uint32_t index_count;
    if (!c || !mesh || !out_vertices || !out_indices || !mesh->vertices || !mesh->indices ||
        rt_mesh3d_safe_vertex_count(mesh) == 0 || rt_mesh3d_safe_index_count(mesh) == 0)
        return 0;
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    index_count = rt_mesh3d_safe_index_count(mesh);
    if ((size_t)vertex_count > SIZE_MAX / sizeof(*vertices) ||
        (size_t)index_count > SIZE_MAX / sizeof(*indices))
        return 0;
    vertex_bytes = (size_t)vertex_count * sizeof(*vertices);
    index_bytes = (size_t)index_count * sizeof(*indices);
    if (vertex_bytes > SIZE_MAX - index_bytes)
        return 0;
    size_t total_bytes = vertex_bytes + index_bytes;
    size_t snapshot_budget = (size_t)RT_CANVAS3D_MESH_SNAPSHOT_FRAME_BYTE_BUDGET;
    if (canvas3d_consume_injected_snapshot_failure(c, total_bytes))
        return 0;
    if (total_bytes > snapshot_budget || c->mesh_snapshot_bytes > snapshot_budget - total_bytes) {
        canvas3d_record_mesh_snapshot_drop(c, total_bytes);
        return 0;
    }
    vertices = (vgfx3d_vertex_t *)malloc(vertex_bytes);
    if (!vertices) {
        canvas3d_record_mesh_snapshot_drop(c, total_bytes);
        return 0;
    }
    indices = (uint32_t *)malloc(index_bytes);
    if (!indices) {
        canvas3d_record_mesh_snapshot_drop(c, total_bytes);
        free(vertices);
        return 0;
    }
    memcpy(vertices, mesh->vertices, vertex_bytes);
    memcpy(indices, mesh->indices, index_bytes);
    if (!canvas3d_track_temp_buffer(c, vertices)) {
        canvas3d_record_mesh_snapshot_drop(c, total_bytes);
        free(vertices);
        free(indices);
        return 0;
    }
    if (!canvas3d_track_temp_buffer(c, indices)) {
        canvas3d_record_mesh_snapshot_drop(c, total_bytes);
        canvas3d_release_tracked_temp_buffer(c, vertices);
        free(indices);
        return 0;
    }
    c->mesh_snapshot_bytes += total_bytes;
    *out_vertices = vertices;
    *out_indices = indices;
    return 1;
}

/// @brief Compute the axis-aligned bounding box of a vertex array (for culling/occlusion).
/// @param vertices Borrowed array of @p vertex_count vertices; may be `NULL`.
/// @param vertex_count Number of elements in @p vertices.
/// @param out_min Output three-element minimum corner.
/// @param out_max Output three-element maximum corner.
/// @details Null or empty input produces zero bounds. Null output pointers are
///   ignored without modifying either destination.
void canvas3d_compute_vertices_aabb(const vgfx3d_vertex_t *vertices,
                                    uint32_t vertex_count,
                                    float out_min[3],
                                    float out_max[3]) {
    if (!out_min || !out_max)
        return;
    if (!vertices || vertex_count == 0) {
        out_min[0] = out_min[1] = out_min[2] = 0.0f;
        out_max[0] = out_max[1] = out_max[2] = 0.0f;
        return;
    }
    vgfx3d_compute_mesh_aabb(vertices, vertex_count, sizeof(vgfx3d_vertex_t), out_min, out_max);
}

/// @brief Snapshot a mesh while subtracting a local-space rebase vector before float upload.
/// @details Camera-relative rendering can move the frame origin into vertex data when the model
///          matrix would otherwise multiply very large positions. `Mesh3D.AddVertex` preserves
///          authored double positions in `positions64`; importer buffers without a sidecar fall
///          back to their existing float positions.
/// @param c Canvas that owns and tracks the copied buffers.
/// @param mesh Borrowed source mesh and optional double-precision position sidecar.
/// @param origin Three-element local-space origin subtracted from every position.
/// @param out_vertices Output receiving the rebased frame-owned vertex copy.
/// @param out_indices Output receiving the unchanged frame-owned index copy.
/// @return Nonzero on success. Returns zero if ordinary snapshotting fails or any
///   rebased component cannot fit in `float`; range failure releases both copies
///   and sets both output pointers to `NULL`.
int canvas3d_snapshot_mesh_geometry_rebased(rt_canvas3d *c,
                                            const rt_mesh3d *mesh,
                                            const double origin[3],
                                            vgfx3d_vertex_t **out_vertices,
                                            uint32_t **out_indices) {
    uint32_t vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    uint32_t index_count = rt_mesh3d_safe_index_count(mesh);
    size_t vertex_bytes = (size_t)vertex_count * sizeof(**out_vertices);
    size_t index_bytes = (size_t)index_count * sizeof(**out_indices);
    if (!canvas3d_snapshot_mesh_geometry(c, mesh, out_vertices, out_indices))
        return 0;
    for (uint32_t i = 0; i < vertex_count; i++) {
        double x = mesh->positions64 ? mesh->positions64[(size_t)i * 3u + 0]
                                     : (double)mesh->vertices[i].pos[0];
        double y = mesh->positions64 ? mesh->positions64[(size_t)i * 3u + 1]
                                     : (double)mesh->vertices[i].pos[1];
        double z = mesh->positions64 ? mesh->positions64[(size_t)i * 3u + 2]
                                     : (double)mesh->vertices[i].pos[2];
        x -= origin[0];
        y -= origin[1];
        z -= origin[2];
        if (!canvas3d_double_fits_float(x) || !canvas3d_double_fits_float(y) ||
            !canvas3d_double_fits_float(z)) {
            canvas3d_release_tracked_mesh_snapshot(
                c, *out_vertices, vertex_bytes, *out_indices, index_bytes);
            *out_vertices = NULL;
            *out_indices = NULL;
            return 0;
        }
        (*out_vertices)[i].pos[0] = (float)x;
        (*out_vertices)[i].pos[1] = (float)y;
        (*out_vertices)[i].pos[2] = (float)z;
    }
    return 1;
}

/// @brief Ensure the per-frame mesh-snapshot cache can hold @p needed entries (grows as needed).
/// @param c Canvas owning the cache entry array.
/// @param needed Nonnegative minimum entry capacity.
/// @return Nonzero when the requested capacity is available; zero for invalid
///   state, arithmetic overflow, or allocation failure. Existing entries survive
///   unsuccessful growth.
int canvas3d_reserve_mesh_snapshot_cache(rt_canvas3d *c, int32_t needed) {
    if (!c)
        return 0;
    if (needed < 0 || c->mesh_snapshot_capacity < 0)
        return 0;
    if (needed <= c->mesh_snapshot_capacity)
        return 1;
    if (c->mesh_snapshot_capacity > INT32_MAX / 2)
        return 0;
    int32_t new_cap = c->mesh_snapshot_capacity == 0 ? 8 : c->mesh_snapshot_capacity * 2;
    while (new_cap < needed) {
        if (new_cap > INT32_MAX / 2)
            return 0;
        new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(*c->mesh_snapshots))
        return 0;
    rt_canvas3d_mesh_snapshot_entry *entries = (rt_canvas3d_mesh_snapshot_entry *)realloc(
        c->mesh_snapshots, (size_t)new_cap * sizeof(*entries));
    if (!entries)
        return 0;
    c->mesh_snapshots = entries;
    c->mesh_snapshot_capacity = new_cap;
    return 1;
}

/// @brief Bind immutable geometry for deferred upload, reusing the current revision when unchanged.
/// @details Heap meshes use their cross-frame retained revision: the canvas takes one reference,
///          so a later source mutation cannot invalidate already queued pointers. Stack/transient
///          meshes retain the legacy per-frame copy path. The frame snapshot table deduplicates
///          both forms by source, source revision, and safe element counts.
/// @param c Canvas owning frame binding state and retained references.
/// @param mesh Borrowed mutable mesh whose current geometry revision is bound.
/// @param mesh_obj Source identity; heap payloads enable retained-revision caching,
///   while other values use a frame-owned geometry copy.
/// @param out_vertices Output receiving immutable vertices valid through frame cleanup.
/// @param out_indices Output receiving immutable indices valid through frame cleanup.
/// @return Nonzero when stable geometry was bound. Returns zero for invalid input,
///   injected failure, unusable source geometry, budget rejection, or allocation
///   failure. Failure to add an already valid copy to the lookup table is nonfatal.
int canvas3d_snapshot_mesh_geometry_cached(rt_canvas3d *c,
                                           rt_mesh3d *mesh,
                                           void *mesh_obj,
                                           vgfx3d_vertex_t **out_vertices,
                                           uint32_t **out_indices) {
    rt_mesh3d_geometry_revision *revision = NULL;
    uint32_t vertex_count;
    uint32_t index_count;
    if (!c || !mesh || !out_vertices || !out_indices)
        return 0;
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    index_count = rt_mesh3d_safe_index_count(mesh);
    if ((size_t)vertex_count > SIZE_MAX / sizeof(vgfx3d_vertex_t) ||
        (size_t)index_count > SIZE_MAX / sizeof(uint32_t) ||
        (size_t)vertex_count * sizeof(vgfx3d_vertex_t) >
            SIZE_MAX - (size_t)index_count * sizeof(uint32_t)) {
        if (canvas3d_consume_injected_snapshot_failure(c, SIZE_MAX))
            return 0;
    } else if (canvas3d_consume_injected_snapshot_failure(
                   c,
                   (size_t)vertex_count * sizeof(vgfx3d_vertex_t) +
                       (size_t)index_count * sizeof(uint32_t))) {
        return 0;
    }
    int can_cache = mesh_obj && rt_heap_is_payload(mesh_obj);
    if (can_cache) {
        int32_t index = canvas3d_find_mesh_snapshot(
            c, mesh_obj, mesh->geometry_revision, vertex_count, index_count);
        if (index >= 0) {
            rt_canvas3d_mesh_snapshot_entry *entry = &c->mesh_snapshots[index];
            *out_vertices = entry->vertices;
            *out_indices = entry->indices;
            return 1;
        }
        revision = rt_mesh3d_get_retained_geometry(mesh);
        if (revision && c->mesh_snapshot_count < INT32_MAX &&
            canvas3d_reserve_mesh_snapshot_cache(c, c->mesh_snapshot_count + 1)) {
            rt_canvas3d_mesh_snapshot_entry *entry = &c->mesh_snapshots[c->mesh_snapshot_count++];
            rt_mesh3d_geometry_revision_retain(revision);
            entry->source = mesh_obj;
            entry->geometry_revision = revision->source_revision;
            entry->vertex_count = revision->vertex_count;
            entry->index_count = revision->index_count;
            entry->vertices = revision->vertices;
            entry->indices = revision->indices;
            entry->tangents_generated = 0;
            entry->retained_revision = revision;
            (void)canvas3d_ensure_mesh_snapshot_hash(c, c->mesh_snapshot_count);
            *out_vertices = revision->vertices;
            *out_indices = revision->indices;
            return 1;
        }
    }
    if (!canvas3d_snapshot_mesh_geometry(c, mesh, out_vertices, out_indices))
        return 0;
    if (can_cache) {
        if (c->mesh_snapshot_count >= INT32_MAX)
            return 1;
        if (!canvas3d_reserve_mesh_snapshot_cache(c, c->mesh_snapshot_count + 1))
            return 1;
        rt_canvas3d_mesh_snapshot_entry *entry = &c->mesh_snapshots[c->mesh_snapshot_count++];
        entry->source = mesh_obj;
        entry->geometry_revision = mesh->geometry_revision;
        entry->vertex_count = vertex_count;
        entry->index_count = index_count;
        entry->vertices = *out_vertices;
        entry->indices = *out_indices;
        entry->tangents_generated = 0;
        entry->retained_revision = NULL;
        (void)canvas3d_ensure_mesh_snapshot_hash(c, c->mesh_snapshot_count);
    }
    return 1;
}

/// @brief Generate tangents directly into a canvas-owned mesh snapshot.
/// @details Builds a temporary Mesh3D facade over @p vertices/@p indices so the existing tangent
///   generator can run without mutating the source mesh. The vertex and index buffers remain owned
///   by the canvas temp-buffer tracker; only the transient facade lives on the stack.
/// @param source Borrowed mesh supplying validated counts and geometry revision.
/// @param vertices Writable frame-owned vertex copy updated with generated tangents.
/// @param indices Borrowed frame-owned index copy used to derive tangent topology.
/// @return Nonzero when tangent generation reports a ready variant; zero for null
///   input or generation failure.
static int canvas3d_generate_cached_snapshot_tangents(const rt_mesh3d *source,
                                                      vgfx3d_vertex_t *vertices,
                                                      uint32_t *indices) {
    rt_mesh3d temp;
    if (!source || !vertices || !indices)
        return 0;
    memset(&temp, 0, sizeof(temp));
    temp.vertices = vertices;
    temp.vertex_count = rt_mesh3d_safe_vertex_count(source);
    temp.vertex_capacity = temp.vertex_count;
    temp.indices = indices;
    temp.index_count = rt_mesh3d_safe_index_count(source);
    temp.index_capacity = temp.index_count;
    temp.geometry_revision = source->geometry_revision;
    temp.transient_geometry_facade = 1;
    rt_mesh3d_calc_tangents_impl(&temp);
    return temp.tangents_ready ? 1 : 0;
}

/// @brief Bind geometry and lazily reuse a persistent generated-tangent vertex variant.
/// @details Heap meshes generate tangents into their immutable retained revision, keyed by the
///          source geometry revision (which covers positions, normals, UVs, and topology). The
///          variant therefore survives frame cleanup and is shared by every backend submission
///          until mutation forks the mesh revision. Stack/transient sources keep the legacy
///          frame-owned snapshot behavior.
/// @param c Canvas owning frame binding state and retained references.
/// @param mesh Borrowed mutable mesh whose geometry needs a tangent-ready binding.
/// @param mesh_obj Source identity; a heap payload permits persistent revision caching.
/// @param out_vertices Output receiving the retained tangent variant or a tangent-modified
///   frame copy, valid through frame cleanup.
/// @param out_indices Output receiving the matching retained or frame-owned indices.
/// @return Nonzero when tangent-ready stable geometry is bound. Returns zero for
///   invalid input, injected failure, snapshot/allocation failure, or unsuccessful
///   tangent generation; temporary copies are released on generation failure.
int canvas3d_snapshot_mesh_geometry_with_tangents_cached(rt_canvas3d *c,
                                                         rt_mesh3d *mesh,
                                                         void *mesh_obj,
                                                         vgfx3d_vertex_t **out_vertices,
                                                         uint32_t **out_indices) {
    rt_mesh3d_geometry_revision *revision = NULL;
    uint32_t vertex_count;
    uint32_t index_count;
    int can_cache;

    if (!c || !mesh || !out_vertices || !out_indices)
        return 0;
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    index_count = rt_mesh3d_safe_index_count(mesh);
    if ((size_t)vertex_count <= SIZE_MAX / sizeof(vgfx3d_vertex_t) &&
        (size_t)index_count <= SIZE_MAX / sizeof(uint32_t) &&
        (size_t)vertex_count * sizeof(vgfx3d_vertex_t) <=
            SIZE_MAX - (size_t)index_count * sizeof(uint32_t) &&
        canvas3d_consume_injected_snapshot_failure(c,
                                                   (size_t)vertex_count * sizeof(vgfx3d_vertex_t) +
                                                       (size_t)index_count * sizeof(uint32_t)))
        return 0;
    can_cache = mesh_obj && rt_heap_is_payload(mesh_obj);
    if (can_cache) {
        int32_t index = canvas3d_find_mesh_snapshot(
            c, mesh_obj, mesh->geometry_revision, vertex_count, index_count);
        if (index >= 0) {
            rt_canvas3d_mesh_snapshot_entry *entry = &c->mesh_snapshots[index];
            if (entry->retained_revision) {
                if (!rt_mesh3d_geometry_revision_ensure_tangents(mesh, entry->retained_revision))
                    return 0;
                entry->tangents_generated = 1;
                *out_vertices = entry->retained_revision->tangent_vertices;
                *out_indices = entry->retained_revision->indices;
                return 1;
            }
            if (!entry->tangents_generated) {
                if (!canvas3d_generate_cached_snapshot_tangents(
                        mesh, entry->vertices, entry->indices))
                    return 0;
                entry->tangents_generated = 1;
            }
            *out_vertices = entry->vertices;
            *out_indices = entry->indices;
            return 1;
        }
        revision = rt_mesh3d_get_retained_geometry(mesh);
        if (revision && rt_mesh3d_geometry_revision_ensure_tangents(mesh, revision) &&
            c->mesh_snapshot_count < INT32_MAX &&
            canvas3d_reserve_mesh_snapshot_cache(c, c->mesh_snapshot_count + 1)) {
            rt_canvas3d_mesh_snapshot_entry *entry = &c->mesh_snapshots[c->mesh_snapshot_count++];
            rt_mesh3d_geometry_revision_retain(revision);
            entry->source = mesh_obj;
            entry->geometry_revision = revision->source_revision;
            entry->vertex_count = revision->vertex_count;
            entry->index_count = revision->index_count;
            entry->vertices = revision->vertices;
            entry->indices = revision->indices;
            entry->tangents_generated = 1;
            entry->retained_revision = revision;
            (void)canvas3d_ensure_mesh_snapshot_hash(c, c->mesh_snapshot_count);
            *out_vertices = revision->tangent_vertices;
            *out_indices = revision->indices;
            return 1;
        }
    }
    if (!canvas3d_snapshot_mesh_geometry(c, mesh, out_vertices, out_indices))
        return 0;
    if (!canvas3d_generate_cached_snapshot_tangents(mesh, *out_vertices, *out_indices)) {
        size_t vertex_bytes = (size_t)vertex_count * sizeof(**out_vertices);
        size_t index_bytes = (size_t)index_count * sizeof(**out_indices);
        canvas3d_release_tracked_mesh_snapshot(
            c, *out_vertices, vertex_bytes, *out_indices, index_bytes);
        *out_vertices = NULL;
        *out_indices = NULL;
        return 0;
    }
    if (can_cache) {
        if (c->mesh_snapshot_count >= INT32_MAX)
            return 1;
        if (!canvas3d_reserve_mesh_snapshot_cache(c, c->mesh_snapshot_count + 1))
            return 1;
        rt_canvas3d_mesh_snapshot_entry *entry = &c->mesh_snapshots[c->mesh_snapshot_count++];
        entry->source = mesh_obj;
        entry->geometry_revision = mesh->geometry_revision;
        entry->vertex_count = vertex_count;
        entry->index_count = index_count;
        entry->vertices = *out_vertices;
        entry->indices = *out_indices;
        entry->tangents_generated = 1;
        entry->retained_revision = NULL;
        (void)canvas3d_ensure_mesh_snapshot_hash(c, c->mesh_snapshot_count);
    }
    return 1;
}

/// @brief Release every immutable mesh revision retained by the frame snapshot table.
/// @details Called before the table's logical count is reset. Per-frame malloc snapshots have a
///          NULL revision pointer and continue to be reclaimed by the ordinary temp-buffer list;
///          retained revisions instead drop their canvas ownership reference here.
/// @param c Canvas whose current frame snapshot entries should relinquish revision ownership.
void canvas3d_release_retained_mesh_revisions(rt_canvas3d *c) {
    if (!c)
        return;
    for (int32_t i = 0; i < c->mesh_snapshot_count; ++i) {
        rt_canvas3d_mesh_snapshot_entry *entry = &c->mesh_snapshots[i];
        if (entry->retained_revision) {
            rt_mesh3d_geometry_revision_release(entry->retained_revision);
            entry->retained_revision = NULL;
        }
    }
}

/// @brief Decide whether a mesh requires a stable deferred geometry binding.
/// @details Heap meshes bind an immutable retained revision (or a frame snapshot if retained
///          allocation fails) so a later mutation cannot alter submitted bytes. Draw-time
///          deformation payloads stay on the original retained mesh object so GPU skinning/morph
///          paths can bind palettes and weights without forcing CPU geometry snapshots.
/// @param mesh Borrowed mesh; must be non-null for a positive decision.
/// @param mesh_obj Candidate runtime object identity.
/// @return Nonzero exactly when both pointers are non-null and @p mesh_obj is a
///   managed heap payload; zero for stack/transient or invalid inputs.
int canvas3d_should_snapshot_geometry(const rt_mesh3d *mesh, void *mesh_obj) {
    if (!mesh || !mesh_obj)
        return 0;
    if (rt_heap_is_payload(mesh_obj))
        return 1;
    return 0;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
