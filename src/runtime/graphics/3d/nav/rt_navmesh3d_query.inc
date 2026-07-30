//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/nav/rt_navmesh3d_query.inc
// Purpose: NavMesh3D A* pathfinding, point/polygon queries, transactional
//   obstacle edits, and test-only query instrumentation. This implementation
//   fragment is included by rt_navmesh3d.c and is not a standalone translation
//   unit.
//
// Key invariants:
//   - Public point inputs are finite and bounded before float conversion.
//   - Failed topology rebuilds preserve the previously published query state.
//   - Path queries never return partially initialized point arrays.
//   - In-flight A* queries exclusively own one slot from a four-workspace pool.
//   - Indexed nearest-point search stops only after a conservative geometric
//     bound proves every unvisited cell unable to improve the result.
//
// Ownership/Lifetime:
//   - Returned path objects are runtime-managed.
//   - Flat point arrays returned by copy helpers are caller-owned.
//
// Links: rt_navmesh3d.c, rt_navmesh3d_build.inc, rt_navmesh3d_bake.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_navmesh3d_query.inc
 * @brief Implements NavMesh3D pathfinding, spatial queries, and dynamic edits.
 *
 * This implementation fragment is included by `rt_navmesh3d.c`. It owns the
 * reusable A* workspaces, corridor smoothing, nearest-surface queries,
 * off-mesh-link metadata, obstacle edits, area queries, tile rebuild entry
 * points, and related diagnostic hooks. It is not a standalone translation
 * unit.
 */

/*==========================================================================
 * A* pathfinding
 *=========================================================================*/

typedef navmesh3d_heap_entry_t heap_entry_t;

/// @brief Claim one independently mutable A* workspace from a navmesh's bounded pool.
/// @details Every slot has its own arrays and ownership flag. The caller yields only when all
///   slots are occupied, allowing the common multi-worker case to run searches concurrently.
/// @param nm Navigation mesh whose workspace pool is queried.
/// @return An exclusively owned slot, or NULL for a null navmesh.
static navmesh3d_path_workspace_t *navmesh3d_path_workspace_acquire(rt_navmesh3d *nm) {
    int spins = 0;
    if (!nm)
        return NULL;
    for (;;) {
        for (int i = 0; i < NAVMESH3D_PATH_WORKSPACE_COUNT; i++) {
            int expected = 0;
            if (rt_atomic_compare_exchange_i32(&nm->path_workspaces[i].in_use,
                                               &expected,
                                               1,
                                               __ATOMIC_ACQ_REL,
                                               __ATOMIC_ACQUIRE)) {
                int active =
                    rt_atomic_fetch_add_i32(&nm->path_active_queries, 1, __ATOMIC_ACQ_REL) + 1;
                int peak = rt_atomic_load_i32(&nm->path_peak_active_queries, __ATOMIC_ACQUIRE);
                while (active > peak &&
                       !rt_atomic_compare_exchange_i32(&nm->path_peak_active_queries,
                                                       &peak,
                                                       active,
                                                       __ATOMIC_ACQ_REL,
                                                       __ATOMIC_ACQUIRE)) {
                }
                return &nm->path_workspaces[i];
            }
        }
        /* A bounded pool deliberately applies back-pressure after four overlapping searches.
         * Briefly spin to catch short queries, then yield to one of the owners. */
        if (++spins >= 64) {
            rt_thread_yield();
            spins = 0;
        }
    }
}

/// @brief Return one exclusively owned A* workspace to its navmesh pool.
/// @param nm Navigation mesh that owns @p workspace.
/// @param workspace Slot returned by `navmesh3d_path_workspace_acquire`.
static void navmesh3d_path_workspace_release(rt_navmesh3d *nm,
                                             navmesh3d_path_workspace_t *workspace) {
    if (!nm || !workspace)
        return;
    rt_atomic_fetch_sub_i32(&nm->path_active_queries, 1, __ATOMIC_ACQ_REL);
    rt_atomic_store_i32(&workspace->in_use, 0, __ATOMIC_RELEASE);
}

/// @brief Grow reusable A* arrays without discarding the previous workspace on allocation failure.
/// @param workspace Exclusively held workspace whose buffers may be enlarged.
/// @param triangle_count Required capacity of the per-triangle cost, parent, and closed arrays.
/// @param heap_capacity Required number of entries in the open-set heap.
/// @return One when every requested buffer is available, or zero for invalid sizes or allocation
/// failure.
static int navmesh3d_reserve_path_workspace(navmesh3d_path_workspace_t *workspace,
                                            int32_t triangle_count,
                                            int32_t heap_capacity) {
    if (!workspace || triangle_count <= 0 || heap_capacity <= 0)
        return 0;
    if (triangle_count > workspace->triangle_capacity) {
        float *g_cost = (float *)malloc((size_t)triangle_count * sizeof(*g_cost));
        int32_t *parent = (int32_t *)malloc((size_t)triangle_count * sizeof(*parent));
        int8_t *closed = (int8_t *)malloc((size_t)triangle_count * sizeof(*closed));
        if (!g_cost || !parent || !closed) {
            free(g_cost);
            free(parent);
            free(closed);
            return 0;
        }
        free(workspace->g_cost);
        free(workspace->parent);
        free(workspace->closed);
        workspace->g_cost = g_cost;
        workspace->parent = parent;
        workspace->closed = closed;
        workspace->triangle_capacity = triangle_count;
    }
    if (heap_capacity > workspace->heap_capacity) {
        heap_entry_t *heap = (heap_entry_t *)malloc((size_t)heap_capacity * sizeof(*heap));
        if (!heap)
            return 0;
        free(workspace->heap);
        workspace->heap = heap;
        workspace->heap_capacity = heap_capacity;
    }
    return 1;
}

/// @brief Atomically publish LastPathCost for concurrent read-only telemetry.
/// @param nm Navigation mesh whose most recent path cost is updated.
/// @param value Non-negative finite cost to publish; invalid values are normalized to zero.
static void navmesh3d_store_last_path_cost(rt_navmesh3d *nm, double value) {
    if (!nm)
        return;
    if (!isfinite(value) || value < 0.0)
        value = 0.0;
    rt_atomic_store_f64(&nm->last_path_cost, &value, __ATOMIC_RELEASE);
}

/// @brief Reset the observed peak number of overlapping A* queries for one navmesh.
/// @details This test-only instrumentation resets the peak to the number of searches currently
///   holding workspace slots, so a caller should normally invoke it while no query is active. It
///   does not alter path results, workspace ownership, or scripting-visible state.
/// @param obj NavMesh3D handle whose concurrency watermark is reset.
void rt_navmesh3d_test_reset_path_peak_concurrency(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm)
        return;
    int active = rt_atomic_load_i32(&nm->path_active_queries, __ATOMIC_ACQUIRE);
    rt_atomic_store_i32(&nm->path_peak_active_queries, active, __ATOMIC_RELEASE);
}

/// @brief Read the peak number of A* searches that simultaneously owned independent workspaces.
/// @details The value is an atomic test-only watermark used to distinguish true overlap from a
///   merely thread-safe but fully serialized implementation. It is not part of the scripting API.
/// @param obj NavMesh3D handle to inspect.
/// @return Peak concurrent query count, or zero for an invalid handle.
int64_t rt_navmesh3d_test_get_path_peak_concurrency(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    return nm ? (int64_t)rt_atomic_load_i32(&nm->path_peak_active_queries, __ATOMIC_ACQUIRE) : 0;
}

/// @brief Push a `(triangle, f-score)` entry onto the A* min-heap and sift up. Comparison
/// uses `f` (g + heuristic) so the heap top is always the lowest-cost open triangle.
/// @note The `*size >= capacity` guard is an (intended) unreachable safety net, not a lossy
/// drop: the caller sizes the heap to `triangle_count*3 + offmesh_links*2 + 8`, and the A*
/// loop closes each triangle at most once, so every directed edge (≤3 triangle neighbours
/// plus the off-mesh links incident on a node) triggers at most one push. Total pushes —
/// and therefore the peak heap occupancy — can never exceed that capacity, so no frontier
/// node is ever silently lost. The guard remains only to keep the function memory-safe were
/// a future caller to under-size the buffer.
/// @param heap Caller-owned heap storage.
/// @param size In/out number of occupied heap entries.
/// @param capacity Maximum number of entries available in @p heap.
/// @param tri Triangle index stored in the new entry.
/// @param f A* priority assigned to the new entry.
static void heap_push(heap_entry_t *heap, int32_t *size, int32_t capacity, int32_t tri, float f) {
    if (*size >= capacity)
        return;
    int32_t i = (*size)++;
    heap[i].tri = tri;
    heap[i].f = f;
    /* Sift up */
    while (i > 0) {
        int32_t p = (i - 1) / 2;
        if (heap[p].f <= heap[i].f)
            break;
        heap_entry_t tmp = heap[p];
        heap[p] = heap[i];
        heap[i] = tmp;
        i = p;
    }
}

/// @brief Pop the lowest-`f` triangle from the A* min-heap and sift down. Returns the
/// triangle index. Caller is responsible for ensuring `*size > 0` (assertion would be
/// nice but this is a hot path; A* main loop guards via `while (size > 0)`).
/// @param heap Caller-owned, non-empty heap storage.
/// @param size In/out number of occupied entries; decremented before returning.
/// @return Triangle index from the removed minimum-priority entry.
static int32_t heap_pop(heap_entry_t *heap, int32_t *size) {
    int32_t result = heap[0].tri;
    heap[0] = heap[--(*size)];
    /* Sift down */
    int32_t i = 0;
    for (;;) {
        int32_t best = i, l = 2 * i + 1, r = 2 * i + 2;
        if (l < *size && heap[l].f < heap[best].f)
            best = l;
        if (r < *size && heap[r].f < heap[best].f)
            best = r;
        if (best == i)
            break;
        heap_entry_t tmp = heap[i];
        heap[i] = heap[best];
        heap[best] = tmp;
        i = best;
    }
    return result;
}

/// @brief Euclidean distance between two triangles' precomputed centroids. Used as the
/// A* heuristic on ordinary triangle-only graphs and as the per-edge step cost when expanding
/// neighbours.
/// @param nm Navigation mesh containing both triangle centroids.
/// @param a First triangle index.
/// @param b Second triangle index.
/// @return Finite centroid distance, or `FLT_MAX` when the computed distance is not finite.
static float centroid_dist(const rt_navmesh3d *nm, int32_t a, int32_t b) {
    const float *ca = nm->triangles[a].centroid;
    const float *cb = nm->triangles[b].centroid;
    float dx = cb[0] - ca[0], dy = cb[1] - ca[1], dz = cb[2] - ca[2];
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    return isfinite(dist) ? dist : FLT_MAX;
}

/// @brief A* heuristic for the current navigation graph.
/// @details Authored off-mesh links can jump between remote polygons for less than their centroid
///   separation, so Euclidean distance is not a lower bound once any such edge exists. In the
///   default strict mode, falling back to zero turns the search into Dijkstra while preserving
///   optimality; NavMesh3D.SetHeuristicMode(1) keeps the Euclidean estimate anyway (Recast-style:
///   much faster on big link-bearing meshes, with possibly slightly suboptimal paths around link
///   shortcuts). Triangle-only navmeshes always use the Euclidean heuristic.
/// @param nm Navigation mesh defining link presence and heuristic policy.
/// @param from Triangle whose remaining cost is estimated.
/// @param goal Destination triangle.
/// @return Zero for strict searches on link-bearing meshes; otherwise the centroid distance.
static float navmesh3d_astar_heuristic(const rt_navmesh3d *nm, int32_t from, int32_t goal) {
    if (!nm)
        return 0.0f;
    if (navmesh3d_read_offmesh_link_count(nm) > 0 && nm->heuristic_mode == 0)
        return 0.0f;
    return centroid_dist(nm, from, goal);
}

/// @brief `NavMesh3D.SetHeuristicMode(mode)` — pathfinding heuristic policy.
/// @details Mode 0 (default, "strict") guarantees optimal paths: any off-mesh link
///   disables the Euclidean estimate (Dijkstra search). Mode 1 ("euclidean") keeps
///   the straight-line estimate even with links — dramatically faster on large
///   navmeshes, at the cost of possibly non-optimal routes where a link shortcut
///   outperforms straight-line distance. Out-of-range modes clamp to 0/1.
/// @param navmesh NavMesh3D handle to update.
/// @param mode Zero for strict optimality, or any nonzero value for Euclidean estimation.
void rt_navmesh3d_set_heuristic_mode(void *navmesh, int64_t mode) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(navmesh, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm)
        return;
    nm->heuristic_mode = mode != 0 ? 1 : 0;
}

/// @brief `NavMesh3D.HeuristicMode` — current pathfinding heuristic policy (0 or 1).
/// @param navmesh NavMesh3D handle to inspect.
/// @return The selected heuristic mode, or zero for an invalid handle.
int64_t rt_navmesh3d_get_heuristic_mode(void *navmesh) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(navmesh, RT_G3D_NAVMESH3D_CLASS_ID);
    return nm ? (int64_t)nm->heuristic_mode : 0;
}

/// @brief A* edge cost between adjacent triangles @p from and @p to: their centroid distance
///   scaled by the average of their traversal costs; FLT_MAX if non-finite (impassable).
/// @param nm Navigation mesh containing the triangles and area costs.
/// @param from Source triangle index.
/// @param to Destination triangle index.
/// @return Finite weighted edge cost, or `FLT_MAX` for an unusable edge.
static float navmesh3d_edge_cost(const rt_navmesh3d *nm, int32_t from, int32_t to) {
    float base = centroid_dist(nm, from, to);
    if (!isfinite(base) || base >= FLT_MAX)
        return FLT_MAX;
    float cost = 0.5f * (navmesh3d_tri_cost(nm, from) + navmesh3d_tri_cost(nm, to));
    float total = base * cost;
    return isfinite(total) ? total : FLT_MAX;
}

/// @brief Traversal cost of an off-mesh link: its 3D length scaled by its sanitized traversal
///   cost; FLT_MAX if non-finite.
/// @param link Off-mesh link to measure.
/// @return Zero for a null link, `FLT_MAX` for a non-finite result, or the weighted link length.
static float navmesh3d_offmesh_cost(const nav_offmesh_link_t *link) {
    if (!link)
        return 0.0f;
    float dx = link->to[0] - link->from[0];
    float dy = link->to[1] - link->from[1];
    float dz = link->to[2] - link->from[2];
    float base = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!isfinite(base))
        return FLT_MAX;
    float total = base * navmesh3d_sanitize_traversal_cost((double)link->traversal_cost);
    return isfinite(total) ? total : FLT_MAX;
}

/// @brief Relax one directed offmesh edge during A* expansion.
/// @details Applies the link traversal cost, skips blocked/closed targets, and pushes the target
///   triangle with its updated f-score when this edge improves the known g-cost.
/// @param nm Navigation mesh being searched.
/// @param cur Triangle currently being expanded.
/// @param next Candidate triangle reached through @p link.
/// @param goal Destination triangle used by the heuristic.
/// @param triangle_count Defensively bounded triangle-array length.
/// @param link Directed off-mesh traversal being relaxed.
/// @param g_cost In/out best-known path cost for every triangle.
/// @param parent In/out predecessor triangle for every improved node.
/// @param closed Read-only closed-set flags for this search.
/// @param heap Open-set heap storage.
/// @param heap_size In/out number of occupied heap entries.
/// @param heap_cap Capacity of @p heap.
static void navmesh3d_astar_relax_offmesh(rt_navmesh3d *nm,
                                          int32_t cur,
                                          int32_t next,
                                          int32_t goal,
                                          int32_t triangle_count,
                                          const nav_offmesh_link_t *link,
                                          float *g_cost,
                                          int32_t *parent,
                                          int8_t *closed,
                                          heap_entry_t *heap,
                                          int32_t *heap_size,
                                          int32_t heap_cap) {
    if (!nm || !link || next < 0 || next >= triangle_count || closed[next] ||
        nm->triangles[next].blocked)
        return;
    float new_g = g_cost[cur] + navmesh3d_offmesh_cost(link);
    if (new_g < g_cost[next]) {
        g_cost[next] = new_g;
        parent[next] = cur;
        heap_push(
            heap, heap_size, heap_cap, next, new_g + navmesh3d_astar_heuristic(nm, next, goal));
    }
}

/// @brief A* search over the navmesh triangle graph (edges + off-mesh links).
/// @details Caller owns the @p g_cost, @p parent, @p closed, and @p heap buffers. The search
/// fills the cost and predecessor arrays while consuming the initialized open and closed sets.
/// @param nm Navigation mesh whose graph is searched.
/// @param start Starting triangle index.
/// @param goal Destination triangle index.
/// @param g_cost In/out best-known path cost per triangle.
/// @param parent In/out predecessor triangle per node.
/// @param closed In/out closed-set flags per triangle.
/// @param heap Caller-owned open-set heap.
/// @param heap_cap Capacity of @p heap.
/// @return One if @p goal was reached, otherwise zero.
static int navmesh3d_astar_search(rt_navmesh3d *nm,
                                  int32_t start,
                                  int32_t goal,
                                  float *g_cost,
                                  int32_t *parent,
                                  int8_t *closed,
                                  heap_entry_t *heap,
                                  int32_t heap_cap) {
    int32_t tc = navmesh3d_read_triangle_count(nm);
    int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
    int32_t heap_size = 0;
    if (!nm || !g_cost || !parent || !closed || !heap || start < 0 || start >= tc || goal < 0 ||
        goal >= tc || heap_cap <= 0)
        return 0;
    g_cost[start] = 0;
    heap_push(heap, &heap_size, heap_cap, start, navmesh3d_astar_heuristic(nm, start, goal));
    while (heap_size > 0) {
        int32_t cur = heap_pop(heap, &heap_size);
        if (cur == goal)
            return 1;
        if (closed[cur])
            continue;
        closed[cur] = 1;

        for (int e = 0; e < 3; e++) {
            int32_t next = nm->triangles[cur].neighbors[e];
            if (next < 0 || next >= tc || closed[next] || nm->triangles[next].blocked)
                continue;

            float new_g = g_cost[cur] + navmesh3d_edge_cost(nm, cur, next);
            if (new_g < g_cost[next]) {
                g_cost[next] = new_g;
                parent[next] = cur;
                heap_push(heap,
                          &heap_size,
                          heap_cap,
                          next,
                          new_g + navmesh3d_astar_heuristic(nm, next, goal));
            }
        }

        int32_t begin = 0;
        int32_t end = 0;
        if (navmesh3d_offmesh_adjacency_range(nm, cur, tc, link_count, &begin, &end)) {
            for (int32_t ei = begin; ei < end; ei++) {
                int32_t li = -1;
                int reverse = 0;
                if (!navmesh3d_unpack_offmesh_edge(nm->offmesh_adjacency_edges[ei], &li, &reverse))
                    continue;
                if (li < 0 || li >= link_count)
                    continue;
                const nav_offmesh_link_t *link = &nm->offmesh_links[li];
                int32_t next = reverse ? link->from_tri : link->to_tri;
                navmesh3d_astar_relax_offmesh(nm,
                                              cur,
                                              next,
                                              goal,
                                              tc,
                                              link,
                                              g_cost,
                                              parent,
                                              closed,
                                              heap,
                                              &heap_size,
                                              heap_cap);
            }
        } else {
            for (int32_t li = 0; li < link_count; li++) {
                const nav_offmesh_link_t *link = &nm->offmesh_links[li];
                int32_t next = -1;
                if (link->from_tri == cur)
                    next = link->to_tri;
                else if (link->bidirectional && link->to_tri == cur)
                    next = link->from_tri;
                navmesh3d_astar_relax_offmesh(nm,
                                              cur,
                                              next,
                                              goal,
                                              tc,
                                              link,
                                              g_cost,
                                              parent,
                                              closed,
                                              heap,
                                              &heap_size,
                                              heap_cap);
            }
        }
    }
    return 0;
}

/// @brief Emit the funnel waypoint(s) for one triangle transition @p ti -> @p tn, advancing @p
/// apex.
/// @details Prefers the shared-edge portal midpoint; falls back to an off-mesh link's endpoints,
///          then to the destination triangle's centroid. Appends into @p points / @p point_count.
/// @param nm Navigation mesh containing the corridor geometry.
/// @param ti Source triangle index.
/// @param tn Destination triangle index.
/// @param apex In/out last emitted waypoint used for duplicate suppression.
/// @param points Caller-owned interleaved XYZ output buffer.
/// @param point_count In/out number of waypoints stored in @p points.
static void navmesh3d_pull_segment(
    rt_navmesh3d *nm, int32_t ti, int32_t tn, float apex[3], double *points, int64_t *point_count) {
    /* Find the shared edge (portal) between consecutive triangles */
    nav_triangle_t *ta = &nm->triangles[ti];
    for (int e = 0; e < 3; e++) {
        if (ta->neighbors[e] == tn) {
            /* Shared edge vertices: v[e] and v[(e+1)%3] */
            int32_t vi0 = ta->v[e];
            int32_t vi1 = ta->v[(e + 1) % 3];
            float *p0 = nm->vertices[vi0].position;
            float *p1 = nm->vertices[vi1].position;
            /* Use the midpoint of the portal as a waypoint.
             * A full funnel algorithm would track left/right boundaries
             * and only emit waypoints at funnel constrictions. For now,
             * the midpoint produces much smoother paths than centroids. */
            float mx = (p0[0] + p1[0]) * 0.5f;
            float my = (p0[1] + p1[1]) * 0.5f;
            float mz = (p0[2] + p1[2]) * 0.5f;
            /* Only add waypoint if it's far enough from the last one
             * (avoids redundant collinear points) */
            float dx = mx - apex[0], dy = my - apex[1], dz = mz - apex[2];
            if (dx * dx + dy * dy + dz * dz > 0.01f) {
                points[*point_count * 3 + 0] = mx;
                points[*point_count * 3 + 1] = my;
                points[*point_count * 3 + 2] = mz;
                (*point_count)++;
                apex[0] = mx;
                apex[1] = my;
                apex[2] = mz;
            }
            return;
        }
    }
    int reverse = 0;
    const nav_offmesh_link_t *link = navmesh3d_find_offmesh_link(nm, ti, tn, &reverse);
    if (link) {
        const float *a = reverse ? link->to : link->from;
        const float *b = reverse ? link->from : link->to;
        float dax = a[0] - apex[0];
        float day = a[1] - apex[1];
        float daz = a[2] - apex[2];
        if (dax * dax + day * day + daz * daz > 0.01f) {
            points[*point_count * 3 + 0] = a[0];
            points[*point_count * 3 + 1] = a[1];
            points[*point_count * 3 + 2] = a[2];
            (*point_count)++;
        }
        points[*point_count * 3 + 0] = b[0];
        points[*point_count * 3 + 1] = b[1];
        points[*point_count * 3 + 2] = b[2];
        (*point_count)++;
        apex[0] = b[0];
        apex[1] = b[1];
        apex[2] = b[2];
    } else {
        /* Fallback to centroid if shared edge not found */
        float *cen = nm->triangles[tn].centroid;
        points[*point_count * 3 + 0] = cen[0];
        points[*point_count * 3 + 1] = cen[1];
        points[*point_count * 3 + 2] = cen[2];
        (*point_count)++;
        apex[0] = cen[0];
        apex[1] = cen[1];
        apex[2] = cen[2];
    }
}

typedef struct {
    float left[3];
    float right[3];
} navmesh3d_portal_t;

/// @brief Signed twice-area of triangle a/b/c in the XZ plane.
/// @details Positive/negative signs describe which side of segment a->b point c lies on and are
///   used by the string-pulling funnel to detect corridor constrictions.
/// @param a First XZ point and origin of the oriented edge.
/// @param b Second XZ point and end of the oriented edge.
/// @param c Point classified relative to the oriented edge.
/// @return Signed twice-area in the XZ plane.
static float navmesh3d_triarea2_xz(const float *a, const float *b, const float *c) {
    float abx = b[0] - a[0];
    float abz = b[2] - a[2];
    float acx = c[0] - a[0];
    float acz = c[2] - a[2];
    return acx * abz - abx * acz;
}

/// @brief Compare two XZ points with a small tolerance for repeated portal vertices.
/// @param a First point.
/// @param b Second point.
/// @return Nonzero when the squared XZ separation is less than `1e-6`, otherwise zero.
static int navmesh3d_vequal_xz(const float *a, const float *b) {
    float dx = b[0] - a[0];
    float dz = b[2] - a[2];
    return dx * dx + dz * dz < 1e-6f;
}

/// @brief Append a funnel corner unless it is nearly identical to the previous apex.
/// @param points Caller-owned interleaved XYZ output buffer.
/// @param point_count In/out number of waypoints stored in @p points.
/// @param corner Candidate corner to append.
/// @param apex In/out current funnel apex, always advanced to @p corner.
static void navmesh3d_append_funnel_corner(double *points,
                                           int64_t *point_count,
                                           const float *corner,
                                           float apex[3]) {
    float dx = corner[0] - apex[0];
    float dy = corner[1] - apex[1];
    float dz = corner[2] - apex[2];
    if (dx * dx + dy * dy + dz * dz > 0.01f) {
        points[*point_count * 3 + 0] = corner[0];
        points[*point_count * 3 + 1] = corner[1];
        points[*point_count * 3 + 2] = corner[2];
        (*point_count)++;
    }
    apex[0] = corner[0];
    apex[1] = corner[1];
    apex[2] = corner[2];
}

/// @brief Extract and orient the shared-edge portal for an adjacent triangle transition.
/// @details The portal is ordered left/right from the supplied path apex in XZ. If the transition
///   is not a normal adjacency edge (for example an offmesh link), returns 0 so the caller can use
///   the existing offmesh fallback.
/// @param nm Navigation mesh containing triangle and vertex data.
/// @param ti Source triangle index.
/// @param tn Adjacent destination triangle index.
/// @param apex Current path apex used to orient the shared edge.
/// @param out Receives the oriented portal endpoints on success.
/// @return One when a finite shared-edge portal was found, otherwise zero.
static int navmesh3d_transition_portal(
    rt_navmesh3d *nm, int32_t ti, int32_t tn, const float *apex, navmesh3d_portal_t *out) {
    if (!nm || !out || ti < 0 || tn < 0 || ti >= nm->triangle_count || tn >= nm->triangle_count)
        return 0;
    nav_triangle_t *ta = &nm->triangles[ti];
    for (int e = 0; e < 3; e++) {
        if (ta->neighbors[e] != tn)
            continue;
        int32_t vi0 = ta->v[e];
        int32_t vi1 = ta->v[(e + 1) % 3];
        if (vi0 < 0 || vi1 < 0 || vi0 >= nm->vertex_count || vi1 >= nm->vertex_count)
            return 0;
        const float *p0 = nm->vertices[vi0].position;
        const float *p1 = nm->vertices[vi1].position;
        if (!navmesh3d_float3_finite(p0) || !navmesh3d_float3_finite(p1))
            return 0;
        if (navmesh3d_triarea2_xz(apex, p0, p1) >= 0.0f) {
            memcpy(out->left, p0, sizeof(out->left));
            memcpy(out->right, p1, sizeof(out->right));
        } else {
            memcpy(out->left, p1, sizeof(out->left));
            memcpy(out->right, p0, sizeof(out->right));
        }
        return 1;
    }
    return 0;
}

/// @brief Run string-pulling over a normal triangle corridor.
/// @details Builds shared-edge portals for the A* triangle sequence and applies the standard
///   simple-stupid funnel in XZ. The final destination point is not appended here; the caller adds
///   the exact requested endpoint after the funnel emits only necessary corners.
/// @param nm Navigation mesh containing corridor geometry.
/// @param seq Ordered triangle indices from path start to path goal.
/// @param count Number of indices in @p seq.
/// @param start Exact world-space path start.
/// @param end Exact world-space path destination.
/// @param points Caller-owned interleaved XYZ output buffer.
/// @param point_count In/out number of waypoints stored in @p points.
/// @return 1 when the corridor used only normal adjacency and the funnel ran, 0 to use fallback.
static int navmesh3d_pull_funnel_corridor(rt_navmesh3d *nm,
                                          const int32_t *seq,
                                          int32_t count,
                                          const float start[3],
                                          const float end[3],
                                          double *points,
                                          int64_t *point_count) {
    if (!nm || !seq || count < 2 || !points || !point_count)
        return 0;
    if ((size_t)count + 1u > SIZE_MAX / sizeof(navmesh3d_portal_t))
        return 0;
    navmesh3d_portal_t *portals =
        (navmesh3d_portal_t *)malloc(((size_t)count + 1u) * sizeof(*portals));
    if (!portals)
        return 0;
    memcpy(portals[0].left, start, sizeof(portals[0].left));
    memcpy(portals[0].right, start, sizeof(portals[0].right));
    float ordering_apex[3] = {start[0], start[1], start[2]};
    for (int32_t i = 0; i < count - 1; i++) {
        if (!navmesh3d_transition_portal(nm, seq[i], seq[i + 1], ordering_apex, &portals[i + 1])) {
            free(portals);
            return 0;
        }
        ordering_apex[0] = nm->triangles[seq[i + 1]].centroid[0];
        ordering_apex[1] = nm->triangles[seq[i + 1]].centroid[1];
        ordering_apex[2] = nm->triangles[seq[i + 1]].centroid[2];
    }
    memcpy(portals[count].left, end, sizeof(portals[count].left));
    memcpy(portals[count].right, end, sizeof(portals[count].right));

    float portal_apex[3] = {start[0], start[1], start[2]};
    float portal_left[3] = {start[0], start[1], start[2]};
    float portal_right[3] = {start[0], start[1], start[2]};
    int32_t apex_index = 0;
    int32_t left_index = 0;
    int32_t right_index = 0;
    int32_t portal_count = count + 1;
    for (int32_t i = 1; i < portal_count; i++) {
        const float *left = portals[i].left;
        const float *right = portals[i].right;

        if (navmesh3d_triarea2_xz(portal_apex, portal_right, right) <= 0.0f) {
            if (navmesh3d_vequal_xz(portal_apex, portal_right) ||
                navmesh3d_triarea2_xz(portal_apex, portal_left, right) > 0.0f) {
                memcpy(portal_right, right, sizeof(portal_right));
                right_index = i;
            } else {
                navmesh3d_append_funnel_corner(points, point_count, portal_left, portal_apex);
                apex_index = left_index;
                memcpy(portal_left, portal_apex, sizeof(portal_left));
                memcpy(portal_right, portal_apex, sizeof(portal_right));
                left_index = apex_index;
                right_index = apex_index;
                i = apex_index;
                continue;
            }
        }

        if (navmesh3d_triarea2_xz(portal_apex, portal_left, left) >= 0.0f) {
            if (navmesh3d_vequal_xz(portal_apex, portal_left) ||
                navmesh3d_triarea2_xz(portal_apex, portal_right, left) < 0.0f) {
                memcpy(portal_left, left, sizeof(portal_left));
                left_index = i;
            } else {
                navmesh3d_append_funnel_corner(points, point_count, portal_right, portal_apex);
                apex_index = right_index;
                memcpy(portal_left, portal_apex, sizeof(portal_left));
                memcpy(portal_right, portal_apex, sizeof(portal_right));
                left_index = apex_index;
                right_index = apex_index;
                i = apex_index;
                continue;
            }
        }
    }
    free(portals);
    return 1;
}

/// @brief Reconstruct the smoothed waypoint list from an A* @p parent chain ending at @p goal.
/// @param nm Navigation mesh containing the traversed corridor.
/// @param parent Predecessor triangle array produced by A*.
/// @param goal Destination triangle at the end of the predecessor chain.
/// @param fx Float X coordinate of the exact starting point.
/// @param fy Float Y coordinate of the exact starting point.
/// @param fz Float Z coordinate of the exact starting point.
/// @param fdx Double-precision X coordinate emitted for the starting waypoint.
/// @param fdy Double-precision Y coordinate emitted for the starting waypoint.
/// @param fdz Double-precision Z coordinate emitted for the starting waypoint.
/// @param tdx Double-precision X coordinate emitted for the destination waypoint.
/// @param tdy Double-precision Y coordinate emitted for the destination waypoint.
/// @param tdz Double-precision Z coordinate emitted for the destination waypoint.
/// @param out_point_count Receives the number of interleaved XYZ points, or zero on failure.
/// @return Newly malloc'd points buffer (caller owns) with @p out_point_count entries, or NULL on
///         allocation failure (then @p out_point_count is 0).
static double *navmesh3d_reconstruct_path(rt_navmesh3d *nm,
                                          const int32_t *parent,
                                          int32_t goal,
                                          float fx,
                                          float fy,
                                          float fz,
                                          double fdx,
                                          double fdy,
                                          double fdz,
                                          double tdx,
                                          double tdy,
                                          double tdz,
                                          int64_t *out_point_count) {
    int64_t point_count = 0;
    int32_t count = 0;
    double *points;
    int32_t *seq;
    int32_t idx;
    int32_t max_points;

    *out_point_count = 0;
    for (int32_t c = goal; c != -1; c = parent[c])
        count++;

    seq = (int32_t *)malloc((size_t)count * sizeof(int32_t));
    if (!seq)
        return NULL;
    idx = count - 1;
    for (int32_t c = goal; c != -1; c = parent[c])
        seq[idx--] = c;

    /* String-pulling emits at most 2 waypoints per triangle transition (a portal midpoint,
     * or an off-mesh link's two endpoints) across `count-1` transitions, plus the start and
     * end points — i.e. at most ~2*count points. `count*3 + 2` is a deliberately generous
     * upper bound with headroom; if the funnel emitter is ever changed to emit more points
     * per segment, keep this >= the real per-segment bound rather than trusting the literal. */
    if (count > (INT32_MAX - 2) / 3) {
        free(seq);
        return NULL;
    }
    max_points = count * 3 + 2;
    points = (double *)malloc((size_t)max_points * 3u * sizeof(double));
    if (!points) {
        free(seq);
        return NULL;
    }
    points[point_count * 3 + 0] = fdx;
    points[point_count * 3 + 1] = fdy;
    points[point_count * 3 + 2] = fdz;
    point_count++;

    /* String-pulling: use a full funnel for ordinary adjacent-triangle corridors.
     * If the corridor contains offmesh/non-adjacent transitions, preserve the
     * endpoint-aware segment fallback so offmesh links still emit their authored
     * traversal endpoints. */
    if (count >= 2) {
        float apex[3] = {fx, fy, fz};
        float end[3] = {(float)tdx, (float)tdy, (float)tdz};
        if (!navmesh3d_pull_funnel_corridor(nm, seq, count, apex, end, points, &point_count)) {
            for (int32_t i = 0; i < count - 1; i++)
                navmesh3d_pull_segment(nm, seq[i], seq[i + 1], apex, points, &point_count);
        }
    } else if (count == 1) {
        float *cen = nm->triangles[seq[0]].centroid;
        points[point_count * 3 + 0] = cen[0];
        points[point_count * 3 + 1] = cen[1];
        points[point_count * 3 + 2] = cen[2];
        point_count++;
    }

    points[point_count * 3 + 0] = tdx;
    points[point_count * 3 + 1] = tdy;
    points[point_count * 3 + 2] = tdz;
    point_count++;
    free(seq);
    *out_point_count = point_count;
    return points;
}

/// @brief Compute a path from @p from to @p to and copy it into a freshly malloc'd flat xyz
///   array.
/// @param obj NavMesh3D handle to query.
/// @param from_v Starting Vec3 in world space.
/// @param to_v Destination Vec3 in world space.
/// @param out_points_xyz Receives a caller-owned interleaved XYZ allocation, or NULL on failure.
/// @return Number of points written to @p out_points_xyz, or zero when no path can be produced.
int64_t rt_navmesh3d_copy_path_points(void *obj,
                                      void *from_v,
                                      void *to_v,
                                      double **out_points_xyz) {
    double *points = NULL;
    int64_t point_count = 0;
    if (out_points_xyz)
        *out_points_xyz = NULL;
    float from_p[3];
    float to_p[3];
    if (!obj || !navmesh3d_read_point3f(from_v, from_p) || !navmesh3d_read_point3f(to_v, to_p))
        return 0;
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    int32_t tc = navmesh3d_read_triangle_count(nm);
    int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
    if (!nm || tc <= 0 || !nm->vertices)
        return 0;
    navmesh3d_store_last_path_cost(nm, 0.0);

    double fdx = from_p[0], fdy = from_p[1], fdz = from_p[2];
    double tdx = to_p[0], tdy = to_p[1], tdz = to_p[2];
    float fx = from_p[0], fy = from_p[1], fz = from_p[2];
    float tx = to_p[0], ty = to_p[1], tz = to_p[2];
    float max_dy = navmesh3d_vertical_tolerance(nm);
    if (!isfinite(fx) || !isfinite(fy) || !isfinite(fz) || !isfinite(tx) || !isfinite(ty) ||
        !isfinite(tz))
        return 0;

    int32_t start = find_tri_with_max_dy(nm, fx, fy, fz, max_dy);
    int32_t goal = find_tri_with_max_dy(nm, tx, ty, tz, max_dy);
    if (start < 0 || goal < 0)
        return 0;

    if (start == goal) {
        points = (double *)malloc(2u * 3u * sizeof(double));
        if (!points)
            return 0;
        double dx = tdx - fdx, dy = tdy - fdy, dz = tdz - fdz;
        navmesh3d_store_last_path_cost(
            nm, sqrt(dx * dx + dy * dy + dz * dz) * (double)navmesh3d_tri_cost(nm, start));
        points[0] = fdx;
        points[1] = fdy;
        points[2] = fdz;
        points[3] = tdx;
        points[4] = tdy;
        points[5] = tdz;
        if (out_points_xyz) {
            *out_points_xyz = points;
        } else {
            free(points);
        }
        return 2;
    }

    /* A* */
    if (tc > INT32_MAX / 3)
        return 0;
    int64_t heap_cap64 = (int64_t)tc * 3 + (int64_t)link_count * 2 + 8;
    if (heap_cap64 <= 0 || heap_cap64 > INT32_MAX)
        return 0;
    int32_t heap_cap = (int32_t)heap_cap64;
    if ((size_t)tc > SIZE_MAX / sizeof(float) || (size_t)tc > SIZE_MAX / sizeof(int32_t) ||
        (size_t)tc > SIZE_MAX / sizeof(int8_t) ||
        (size_t)heap_cap > SIZE_MAX / sizeof(heap_entry_t))
        return 0;
    navmesh3d_path_workspace_t *workspace = navmesh3d_path_workspace_acquire(nm);
    if (!workspace || !navmesh3d_reserve_path_workspace(workspace, tc, heap_cap)) {
        navmesh3d_path_workspace_release(nm, workspace);
        return 0;
    }
    float *g_cost = workspace->g_cost;
    int32_t *parent = workspace->parent;
    int8_t *closed = workspace->closed;
    heap_entry_t *heap = workspace->heap;
    memset(closed, 0, (size_t)tc * sizeof(*closed));
    memset(parent, -1, (size_t)tc * sizeof(int32_t));
    for (int32_t i = 0; i < tc; i++)
        g_cost[i] = FLT_MAX;

    if (navmesh3d_astar_search(nm, start, goal, g_cost, parent, closed, heap, heap_cap)) {
        navmesh3d_store_last_path_cost(nm, (double)g_cost[goal]);
        points = navmesh3d_reconstruct_path(
            nm, parent, goal, fx, fy, fz, fdx, fdy, fdz, tdx, tdy, tdz, &point_count);
    }

    navmesh3d_path_workspace_release(nm, workspace);
    if (out_points_xyz)
        *out_points_xyz = points;
    else
        free(points);
    return point_count;
}

/// @brief Compute a path from `from_v` to `to_v` (both Vec3 world-space points). Returns a
/// Seq of Vec3 waypoints, including endpoints. Empty Seq if either endpoint is off-mesh or
/// no connecting path exists.
/// @param obj NavMesh3D handle to query.
/// @param from_v Starting Vec3 in world space.
/// @param to_v Destination Vec3 in world space.
/// @return Newly allocated Path3D containing the waypoints, or NULL when no path exists.
void *rt_navmesh3d_find_path(void *obj, void *from_v, void *to_v) {
    double *points = NULL;
    int64_t point_count = rt_navmesh3d_copy_path_points(obj, from_v, to_v, &points);
    if (point_count <= 0 || !points)
        return NULL;

    void *path = rt_path3d_new();
    if (!path) {
        free(points);
        return NULL;
    }
    for (int64_t i = 0; i < point_count; i++) {
        void *point = rt_vec3_new(points[i * 3 + 0], points[i * 3 + 1], points[i * 3 + 2]);
        rt_path3d_add_point(path, point);
        navmesh3d_release_local(point);
    }
    free(points);
    return path;
}

/// @brief Compute a path and wrap the optional Path3D in an Option.
/// @details Keeps the exact path-generation behavior of `rt_navmesh3d_find_path`.
///          Unreachable endpoints, invalid inputs, and off-mesh positions are
///          represented as `None`.
/// @param obj NavMesh3D handle.
/// @param from_v Starting Vec3 in world space.
/// @param to_v Target Vec3 in world space.
/// @return `Some(Path3D)` when a path exists, otherwise `None`.
void *rt_navmesh3d_find_path_option(void *obj, void *from_v, void *to_v) {
    void *path = rt_navmesh3d_find_path(obj, from_v, to_v);
    if (!path)
        return rt_option_none();
    void *option = rt_option_some(path);
    navmesh3d_release_local(path);
    return option;
}

/// @brief Mutable nearest-surface result accumulated by indexed SamplePosition queries.
typedef struct {
    float query_x;
    float query_y;
    float query_z;
    float best_distance_sq;
    float best_vertical_distance;
    float best_x;
    float best_y;
    float best_z;
    int32_t best_triangle;
    int32_t triangle_probes;
} navmesh3d_sample_search_t;

/// @brief Test one triangle and update the nearest XZ surface projection with deterministic ties.
/// @details Blocked triangles and invalid indices are ignored. Equal horizontal distances prefer
///   the smaller vertical delta and then the lower triangle index, making results independent of
///   the order in which expanding grid cells happen to expose candidates.
/// @param nm Navigation mesh supplying triangles and vertices.
/// @param triangle_index Candidate triangle index.
/// @param search In/out nearest-point accumulator and probe counter.
static void navmesh3d_sample_test_triangle(const rt_navmesh3d *nm,
                                           int32_t triangle_index,
                                           navmesh3d_sample_search_t *search) {
    float cx;
    float cy;
    float cz;
    float dx;
    float dy;
    float dz;
    float distance_sq;
    int better;
    if (!nm || !search || triangle_index < 0 || triangle_index >= nm->triangle_count)
        return;
    if (search->triangle_probes < INT32_MAX)
        search->triangle_probes++;
    const nav_triangle_t *triangle = &nm->triangles[triangle_index];
    if (triangle->blocked)
        return;
    const float *v0 = nm->vertices[triangle->v[0]].position;
    const float *v1 = nm->vertices[triangle->v[1]].position;
    const float *v2 = nm->vertices[triangle->v[2]].position;
    closest_point_on_tri_xz(search->query_x, search->query_z, v0, v1, v2, &cx, &cz);
    cy = triangle_y_at_xz(cx, cz, v0, v1, v2);
    dx = search->query_x - cx;
    dz = search->query_z - cz;
    dy = fabsf(search->query_y - cy);
    distance_sq = dx * dx + dz * dz;
    if (!isfinite(distance_sq) || !isfinite(dy) || !isfinite(cy))
        return;
    better = distance_sq < search->best_distance_sq;
    if (!better && fabsf(distance_sq - search->best_distance_sq) <= 1e-6f) {
        better = dy < search->best_vertical_distance;
        if (!better && fabsf(dy - search->best_vertical_distance) <= 1e-6f)
            better = search->best_triangle < 0 || triangle_index < search->best_triangle;
    }
    if (!better)
        return;
    search->best_distance_sq = distance_sq;
    search->best_vertical_distance = dy;
    search->best_x = cx;
    search->best_y = cy;
    search->best_z = cz;
    search->best_triangle = triangle_index;
}

/// @brief Return a lower bound on distance from the query to every cell outside a visited box.
/// @details The expanding search has visited every cell in the inclusive rectangle. Any unvisited
///   triangle reference lies beyond one of its four interior boundaries, so the minimum squared
///   one-axis separation is a conservative lower bound on its possible XZ point distance.
/// @param nm Navigation mesh defining the uniform grid.
/// @param search Query coordinates and current result.
/// @param min_cell_x Inclusive visited minimum X cell.
/// @param max_cell_x Inclusive visited maximum X cell.
/// @param min_cell_z Inclusive visited minimum Z cell.
/// @param max_cell_z Inclusive visited maximum Z cell.
/// @return Conservative squared distance, or FLT_MAX when every grid cell is visited.
static float navmesh3d_sample_unvisited_lower_bound_sq(const rt_navmesh3d *nm,
                                                       const navmesh3d_sample_search_t *search,
                                                       int32_t min_cell_x,
                                                       int32_t max_cell_x,
                                                       int32_t min_cell_z,
                                                       int32_t max_cell_z) {
    double cell_size;
    double lower_bound = DBL_MAX;
    if (!nm || !search || !(nm->qgrid_inv_cell > 0.0))
        return 0.0f;
    cell_size = 1.0 / nm->qgrid_inv_cell;
    if (min_cell_x > 0) {
        double boundary = nm->qgrid_min_x + (double)min_cell_x * cell_size;
        double separation = search->query_x > boundary ? search->query_x - boundary : 0.0;
        if (separation < lower_bound)
            lower_bound = separation;
    }
    if (max_cell_x + 1 < nm->qgrid_nx) {
        double boundary = nm->qgrid_min_x + (double)(max_cell_x + 1) * cell_size;
        double separation = search->query_x < boundary ? boundary - search->query_x : 0.0;
        if (separation < lower_bound)
            lower_bound = separation;
    }
    if (min_cell_z > 0) {
        double boundary = nm->qgrid_min_z + (double)min_cell_z * cell_size;
        double separation = search->query_z > boundary ? search->query_z - boundary : 0.0;
        if (separation < lower_bound)
            lower_bound = separation;
    }
    if (max_cell_z + 1 < nm->qgrid_nz) {
        double boundary = nm->qgrid_min_z + (double)(max_cell_z + 1) * cell_size;
        double separation = search->query_z < boundary ? boundary - search->query_z : 0.0;
        if (separation < lower_bound)
            lower_bound = separation;
    }
    if (lower_bound == DBL_MAX)
        return FLT_MAX;
    if (!isfinite(lower_bound) || lower_bound < 0.0 || lower_bound > sqrt((double)FLT_MAX))
        return FLT_MAX;
    return (float)(lower_bound * lower_bound);
}

/// @brief Scan one indexed grid cell into a nearest-surface accumulator.
/// @param nm Navigation mesh supplying the CSR cell index.
/// @param cell_x In-bounds X cell.
/// @param cell_z In-bounds Z cell.
/// @param search In/out nearest-point state.
/// @return 1 on a valid bounded cell scan, or 0 when corrupt metadata or the per-query reference
///   budget requires the bounded linear fallback.
static int navmesh3d_sample_scan_cell(const rt_navmesh3d *nm,
                                      int32_t cell_x,
                                      int32_t cell_z,
                                      navmesh3d_sample_search_t *search) {
    size_t cell;
    int32_t begin;
    int32_t end;
    if (!nm || !search || cell_x < 0 || cell_z < 0 || cell_x >= nm->qgrid_nx ||
        cell_z >= nm->qgrid_nz)
        return 0;
    cell = (size_t)cell_z * (size_t)nm->qgrid_nx + (size_t)cell_x;
    begin = nm->qgrid_starts[cell];
    end = nm->qgrid_starts[cell + 1u];
    if (begin < 0 || end < begin || end > (int32_t)NAVMESH3D_QUERY_GRID_MAX_REFERENCES)
        return 0;
    if (end - begin > NAVMESH3D_SAMPLE_MAX_GRID_REFERENCES - search->triangle_probes)
        return 0;
    for (int32_t k = begin; k < end; k++)
        navmesh3d_sample_test_triangle(nm, nm->qgrid_tris[k], search);
    return 1;
}

/// @brief Find the nearest surface by expanding through spatial-index cell rings.
/// @details The search begins in the query's cell (or the nearest boundary cell for an external
///   query), visits each rectangular ring, and stops once the best distance is strictly below the
///   conservative distance to every unvisited cell. Cell/reference budgets bound pathological
///   replication; exceeding either requests the exact linear fallback.
/// @param nm Navigation mesh with a complete query grid.
/// @param search In/out nearest-point state.
/// @return 1 when the indexed search completed exactly, or 0 when the caller must use fallback.
static int navmesh3d_sample_search_grid(const rt_navmesh3d *nm, navmesh3d_sample_search_t *search) {
    int32_t origin_x;
    int32_t origin_z;
    int32_t max_ring;
    int32_t cells_scanned = 0;
    if (!nm || !search || nm->qgrid_nx <= 0 || nm->qgrid_nz <= 0 || !nm->qgrid_starts ||
        !nm->qgrid_tris || !isfinite(nm->qgrid_min_x) || !isfinite(nm->qgrid_min_z) ||
        !isfinite(nm->qgrid_inv_cell) || nm->qgrid_inv_cell <= 0.0)
        return 0;
    origin_x = navmesh3d_grid_axis_clamped(
        (double)search->query_x, nm->qgrid_min_x, nm->qgrid_inv_cell, nm->qgrid_nx);
    origin_z = navmesh3d_grid_axis_clamped(
        (double)search->query_z, nm->qgrid_min_z, nm->qgrid_inv_cell, nm->qgrid_nz);
    max_ring = nm->qgrid_nx > nm->qgrid_nz ? nm->qgrid_nx : nm->qgrid_nz;
    for (int32_t ring = 0; ring < max_ring; ring++) {
        int32_t left = origin_x - ring;
        int32_t right = origin_x + ring;
        int32_t top = origin_z - ring;
        int32_t bottom = origin_z + ring;
        int32_t min_x = origin_x - ring;
        int32_t max_x = origin_x + ring;
        int32_t min_z = origin_z - ring;
        int32_t max_z = origin_z + ring;
        if (min_x < 0)
            min_x = 0;
        if (max_x >= nm->qgrid_nx)
            max_x = nm->qgrid_nx - 1;
        if (min_z < 0)
            min_z = 0;
        if (max_z >= nm->qgrid_nz)
            max_z = nm->qgrid_nz - 1;
        if (left >= 0) {
            for (int32_t z = min_z; z <= max_z; z++) {
                if (++cells_scanned > NAVMESH3D_SAMPLE_MAX_GRID_CELLS ||
                    !navmesh3d_sample_scan_cell(nm, left, z, search))
                    return 0;
            }
        }
        if (right < nm->qgrid_nx && right != left) {
            for (int32_t z = min_z; z <= max_z; z++) {
                if (++cells_scanned > NAVMESH3D_SAMPLE_MAX_GRID_CELLS ||
                    !navmesh3d_sample_scan_cell(nm, right, z, search))
                    return 0;
            }
        }
        if (top >= 0) {
            for (int32_t x = min_x; x <= max_x; x++) {
                if (x == left || x == right)
                    continue;
                if (++cells_scanned > NAVMESH3D_SAMPLE_MAX_GRID_CELLS ||
                    !navmesh3d_sample_scan_cell(nm, x, top, search))
                    return 0;
            }
        }
        if (bottom < nm->qgrid_nz && bottom != top) {
            for (int32_t x = min_x; x <= max_x; x++) {
                if (x == left || x == right)
                    continue;
                if (++cells_scanned > NAVMESH3D_SAMPLE_MAX_GRID_CELLS ||
                    !navmesh3d_sample_scan_cell(nm, x, bottom, search))
                    return 0;
            }
        }
        if (search->best_triangle >= 0) {
            float lower_bound =
                navmesh3d_sample_unvisited_lower_bound_sq(nm, search, min_x, max_x, min_z, max_z);
            if (search->best_distance_sq + 1e-6f < lower_bound)
                return 1;
        }
        if (min_x == 0 && max_x == nm->qgrid_nx - 1 && min_z == 0 && max_z == nm->qgrid_nz - 1)
            return 1;
    }
    return 1;
}

/// @brief Record SamplePosition query instrumentation atomically.
/// @param nm Navigation mesh receiving the metrics.
/// @param triangle_probes Number of triangle references evaluated by the completed query.
/// @param used_fallback Nonzero when the exact bounded linear fallback ran.
static void navmesh3d_sample_record_metrics(rt_navmesh3d *nm,
                                            int32_t triangle_probes,
                                            int used_fallback) {
    if (!nm)
        return;
    rt_atomic_store_i32(&nm->sample_last_triangle_probes, triangle_probes, __ATOMIC_RELEASE);
    rt_atomic_store_i32(&nm->sample_last_used_fallback, used_fallback ? 1 : 0, __ATOMIC_RELEASE);
}

/// @brief Snap `point` (Vec3) onto the nearest navmesh triangle and return the projected
/// world-space position. Useful for pinning agent positions to walkable surface after
/// teleports. Returns the input unchanged if no triangle is found nearby.
/// @param obj NavMesh3D handle to query.
/// @param point Vec3 world-space position to project.
/// @return Newly allocated projected Vec3; invalid inputs produce the origin, while an empty
/// navmesh produces a sanitized copy of @p point.
void *rt_navmesh3d_sample_position(void *obj, void *point) {
    float query[3];
    if (!obj || !navmesh3d_read_point3f(point, query))
        return rt_vec3_new(0, 0, 0);
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm)
        return rt_vec3_new(0, 0, 0);
    float px = query[0], py = query[1], pz = query[2];
    if (nm->triangle_count <= 0 || !nm->triangles || !nm->vertices)
        return rt_vec3_new(px, py, pz);

    navmesh3d_sample_record_metrics(nm, 0, 0);

    int32_t tri = find_tri(nm, px, py, pz);
    if (tri >= 0) {
        nav_triangle_t *t = &nm->triangles[tri];
        const float *v0 = nm->vertices[t->v[0]].position;
        const float *v1 = nm->vertices[t->v[1]].position;
        const float *v2 = nm->vertices[t->v[2]].position;
        return rt_vec3_new(px, triangle_y_at_xz(px, pz, v0, v1, v2), pz);
    }

    /* Find the nearest point on the triangle surface in XZ through expanding grid rings. */
    navmesh3d_sample_search_t search = {px, py, pz, FLT_MAX, FLT_MAX, px, py, pz, -1, 0};
    int used_fallback = !navmesh3d_sample_search_grid(nm, &search);
    if (used_fallback) {
        /* The grid may be absent by policy or a pathological replicated query may hit its fixed
         * budget. A whole-mesh pass is still exact and is bounded by the validated triangle count.
         */
        search.best_distance_sq = FLT_MAX;
        search.best_vertical_distance = FLT_MAX;
        search.best_triangle = -1;
        for (int32_t i = 0; i < nm->triangle_count; i++)
            navmesh3d_sample_test_triangle(nm, i, &search);
    }
    navmesh3d_sample_record_metrics(nm, search.triangle_probes, used_fallback);
    return rt_vec3_new(search.best_x, search.best_y, search.best_z);
}

/// @brief Read the triangle-reference probe count from the most recent SamplePosition query.
/// @details This atomic, test-only metric includes duplicate grid references and any fallback
///   probes, making it a conservative measure of query work. It is not scripting-visible.
/// @param obj NavMesh3D handle to inspect.
/// @return Non-negative probe count, or zero for an invalid handle.
int64_t rt_navmesh3d_test_get_last_sample_probe_count(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    return nm ? (int64_t)rt_atomic_load_i32(&nm->sample_last_triangle_probes, __ATOMIC_ACQUIRE) : 0;
}

/// @brief Report whether the most recent SamplePosition query used its bounded linear fallback.
/// @param obj NavMesh3D handle to inspect.
/// @return One after fallback, otherwise zero (including invalid handles).
int8_t rt_navmesh3d_test_get_last_sample_used_fallback(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    return nm && rt_atomic_load_i32(&nm->sample_last_used_fallback, __ATOMIC_ACQUIRE) ? 1 : 0;
}

/// @brief Returns 1 if `point` (Vec3) lies within (or near) any walkable triangle.
/// @param obj NavMesh3D handle to query.
/// @param point Vec3 world-space position to classify.
/// @return One when @p point resolves to a walkable triangle within vertical tolerance, otherwise
/// zero.
int8_t rt_navmesh3d_is_walkable(void *obj, void *point) {
    float query[3];
    if (!obj || !navmesh3d_read_point3f(point, query))
        return 0;
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm || nm->triangle_count <= 0 || !nm->triangles || !nm->vertices)
        return 0;
    float px = query[0], py = query[1], pz = query[2];
    return find_tri_with_max_dy(nm, px, py, pz, navmesh3d_vertical_tolerance(nm)) >= 0 ? 1 : 0;
}

/// @brief Number of walkable triangles in the baked navmesh.
/// @param obj NavMesh3D handle to inspect.
/// @return Number of non-blocked triangles, or zero for an invalid or uninitialized mesh.
int64_t rt_navmesh3d_get_triangle_count(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm || !nm->triangles)
        return 0;
    /* Public count is the walkable (non-carved) set; blocked triangles linger in the array only
     * so per-tile rebuilds can flip them back without recompacting. */
    int64_t walkable = 0;
    int32_t triangle_count = navmesh3d_read_triangle_count(nm);
    for (int32_t i = 0; i < triangle_count; i++)
        if (!nm->triangles[i].blocked)
            walkable++;
    return walkable;
}

/// @brief Total accumulated cost of the most recent successful path query, or 0 if none has run.
/// @param obj NavMesh3D handle to inspect.
/// @return Atomically loaded non-negative path cost, or zero for invalid state.
double rt_navmesh3d_get_last_path_cost(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    double value = 0.0;
    if (nm)
        rt_atomic_load_f64(&nm->last_path_cost, &value, __ATOMIC_ACQUIRE);
    return (isfinite(value) && value >= 0.0) ? value : 0.0;
}

/// @brief Add an authored off-mesh traversal link between two walkable points.
/// @param obj NavMesh3D handle to modify.
/// @param from Vec3 world-space start endpoint.
/// @param to Vec3 world-space destination endpoint.
/// @param bidirectional Nonzero to permit traversal in both directions.
/// @return One when the endpoints resolve to triangles and the link is appended, otherwise zero.
int8_t rt_navmesh3d_add_offmesh_link(void *obj, void *from, void *to, int8_t bidirectional) {
    float from_p[3];
    float to_p[3];
    if (!obj || !navmesh3d_read_point3f(from, from_p) || !navmesh3d_read_point3f(to, to_p))
        return 0;
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm || nm->triangle_count <= 0 || !nm->triangles || !nm->vertices)
        return 0;

    float fx = from_p[0], fy = from_p[1], fz = from_p[2];
    float tx = to_p[0], ty = to_p[1], tz = to_p[2];

    float max_dy = navmesh3d_vertical_tolerance(nm);
    int32_t from_tri = find_tri_with_max_dy(nm, fx, fy, fz, max_dy);
    int32_t to_tri = find_tri_with_max_dy(nm, tx, ty, tz, max_dy);
    if (from_tri < 0 || to_tri < 0)
        return 0;

    if (!navmesh3d_array_storage_valid(
            nm->offmesh_links, nm->offmesh_link_count, nm->offmesh_link_capacity) ||
        nm->offmesh_link_count == INT32_MAX)
        return 0;
    if (nm->offmesh_link_count >= nm->offmesh_link_capacity) {
        int32_t new_cap = 4;
        if (nm->offmesh_link_capacity > 0) {
            if (nm->offmesh_link_capacity > INT32_MAX / 2)
                return 0;
            new_cap = nm->offmesh_link_capacity * 2;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(nav_offmesh_link_t))
            return 0;
        nav_offmesh_link_t *links = (nav_offmesh_link_t *)realloc(
            nm->offmesh_links, (size_t)new_cap * sizeof(nav_offmesh_link_t));
        if (!links)
            return 0;
        nm->offmesh_links = links;
        nm->offmesh_link_capacity = new_cap;
    }

    nav_offmesh_link_t *link = &nm->offmesh_links[nm->offmesh_link_count++];
    link->from[0] = fx;
    link->from[1] = fy;
    link->from[2] = fz;
    link->to[0] = tx;
    link->to[1] = ty;
    link->to[2] = tz;
    link->from_tri = from_tri;
    link->to_tri = to_tri;
    link->bidirectional = bidirectional != 0 ? 1 : 0;
    link->state_flags = 0;
    link->traversal_cost = 1.0f;
    link->kind = NULL;
    (void)navmesh3d_build_offmesh_adjacency(nm);
    return 1;
}

/// @brief Attach kind/cost/state metadata to an authored off-mesh link by index.
/// @param obj NavMesh3D handle to modify.
/// @param index Zero-based authored link index.
/// @param kind Runtime string copied by reference; an empty string clears the stored kind.
/// @param traversal_cost Cost multiplier sanitized before storage.
/// @param state_flags Opaque state flags associated with the link.
/// @return One when the link and string are valid and metadata is stored, otherwise zero.
int8_t rt_navmesh3d_set_offmesh_link_metadata(
    void *obj, int64_t index, rt_string kind, double traversal_cost, int64_t state_flags) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
    if (!nm || index < 0 || index >= link_count || !kind || !rt_string_is_handle(kind))
        return 0;
    nav_offmesh_link_t *link = &nm->offmesh_links[(int32_t)index];
    if (link->kind) {
        rt_string_unref(link->kind);
        link->kind = NULL;
    }
    const char *kind_bytes = rt_string_cstr(kind);
    if (kind_bytes && kind_bytes[0] != '\0')
        link->kind = rt_string_ref(kind);
    link->traversal_cost = navmesh3d_sanitize_traversal_cost(traversal_cost);
    link->state_flags = state_flags;
    return 1;
}

/// @brief Read the kind-string metadata of an authored off-mesh link by index.
/// @param obj NavMesh3D handle to inspect.
/// @param index Zero-based authored link index.
/// @return Referenced runtime kind string, or an empty constant string for an invalid index or
/// unset kind.
rt_string rt_navmesh3d_get_offmesh_link_kind(void *obj, int64_t index) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
    if (!nm || index < 0 || index >= link_count)
        return rt_const_cstr("");
    nav_offmesh_link_t *link = &nm->offmesh_links[(int32_t)index];
    return (link->kind && rt_string_is_handle(link->kind)) ? rt_string_ref(link->kind)
                                                           : rt_const_cstr("");
}

/// @brief Read the sanitized pathfinding traversal cost of an authored off-mesh link by index
///   (0 for an out-of-range index).
/// @param obj NavMesh3D handle to inspect.
/// @param index Zero-based authored link index.
/// @return Sanitized traversal multiplier, or zero for an invalid index.
double rt_navmesh3d_get_offmesh_link_traversal_cost(void *obj, int64_t index) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
    if (!nm || index < 0 || index >= link_count)
        return 0.0;
    return (double)navmesh3d_sanitize_traversal_cost(
        (double)nm->offmesh_links[(int32_t)index].traversal_cost);
}

/// @brief Read the state-flag metadata of an authored off-mesh link by index.
/// @param obj NavMesh3D handle to inspect.
/// @param index Zero-based authored link index.
/// @return Stored state flags, or zero for an invalid index.
int64_t rt_navmesh3d_get_offmesh_link_state(void *obj, int64_t index) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
    if (!nm || index < 0 || index >= link_count)
        return 0;
    return nm->offmesh_links[(int32_t)index].state_flags;
}

/// @brief Read and sanitize an obstacle AABB from two Vec3 handles into float min/max arrays.
/// @details Rejects non-Vec3 or non-finite inputs, orders each axis (min<=max), and clamps to the
///          float range. Returns 1 on success, 0 if the bounds are unusable.
/// @param min_v First Vec3 corner of the obstacle bounds.
/// @param max_v Opposite Vec3 corner of the obstacle bounds.
/// @param out_min Receives the component-wise ordered lower bounds.
/// @param out_max Receives the component-wise ordered upper bounds.
/// @return One when both corners are valid and outputs are populated, otherwise zero.
static int navmesh3d_read_obstacle_bounds(void *min_v,
                                          void *max_v,
                                          float *out_min,
                                          float *out_max) {
    if (!out_min || !out_max)
        return 0;
    float min_f[3];
    float max_f[3];
    if (!navmesh3d_read_point3f(min_v, min_f) || !navmesh3d_read_point3f(max_v, max_f))
        return 0;
    for (int32_t i = 0; i < 3; i++) {
        double lo = min_f[i] < max_f[i] ? min_f[i] : max_f[i];
        double hi = min_f[i] < max_f[i] ? max_f[i] : min_f[i];
        out_min[i] = (float)lo;
        out_max[i] = (float)hi;
    }
    return 1;
}

/// @brief Whether obstacles can be edited: the navmesh must retain its unfiltered source triangles.
/// @param nm Navigation mesh to inspect.
/// @return Nonzero when both live and preserved source triangles are available.
static int navmesh3d_can_edit_obstacles(const rt_navmesh3d *nm) {
    return nm && nm->source_triangles && nm->triangles && nm->source_triangle_count > 0;
}

/// @brief Add a coarse AABB obstacle and immediately refilter the walkable triangle set.
/// @param obj NavMesh3D handle to modify.
/// @param min_v First Vec3 corner of the obstacle bounds.
/// @param max_v Opposite Vec3 corner of the obstacle bounds.
/// @return One when the obstacle is stored and applied, otherwise zero.
int8_t rt_navmesh3d_add_obstacle(void *obj, void *min_v, void *max_v) {
    if (!obj)
        return 0;
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!navmesh3d_can_edit_obstacles(nm))
        return 0;

    float omin[3];
    float omax[3];
    if (!navmesh3d_read_obstacle_bounds(min_v, max_v, omin, omax))
        return 0;

    if (!navmesh3d_array_storage_valid(nm->obstacles, nm->obstacle_count, nm->obstacle_capacity) ||
        nm->obstacle_count == INT32_MAX)
        return 0;
    if (nm->obstacle_count >= nm->obstacle_capacity) {
        int32_t new_cap = 4;
        if (nm->obstacle_capacity > 0) {
            if (nm->obstacle_capacity > INT32_MAX / 2)
                return 0;
            new_cap = nm->obstacle_capacity * 2;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(nav_obstacle_t))
            return 0;
        nav_obstacle_t *obstacles =
            (nav_obstacle_t *)realloc(nm->obstacles, (size_t)new_cap * sizeof(nav_obstacle_t));
        if (!obstacles)
            return 0;
        nm->obstacles = obstacles;
        nm->obstacle_capacity = new_cap;
    }

    nav_obstacle_t *obstacle = &nm->obstacles[nm->obstacle_count++];
    memcpy(obstacle->min, omin, sizeof(omin));
    memcpy(obstacle->max, omax, sizeof(omax));
    /* Tiled bake: re-carve only the tiles the obstacle AABB overlaps. The triangle set, adjacency,
     * and query grid are untouched — only the in-tile `blocked` flags flip. O(overlapped tiles). */
    if (nm->tile_size > 0.0 && nm->qgrid_nx > 0) {
        navmesh3d_reflag_obstacle_tiles(nm, omin, omax);
        return 1;
    }
    if (!navmesh3d_apply_slope_filter(nm)) {
        nm->obstacle_count--;
        return 0;
    }
    return 1;
}

/// @brief Remove a coarse AABB obstacle and immediately refilter the walkable triangle set.
/// @param obj NavMesh3D handle to modify.
/// @param index Zero-based obstacle index.
/// @return One when the obstacle is removed and the mesh state is updated, otherwise zero.
int8_t rt_navmesh3d_remove_obstacle(void *obj, int64_t index) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    int32_t obstacle_count = navmesh3d_read_obstacle_count(nm);
    if (!navmesh3d_can_edit_obstacles(nm) ||
        !navmesh3d_array_storage_valid(nm->obstacles, nm->obstacle_count, nm->obstacle_capacity) ||
        index < 0 || index >= obstacle_count)
        return 0;
    int32_t remove_at = (int32_t)index;
    nav_obstacle_t removed = nm->obstacles[remove_at];
    for (int32_t i = remove_at; i < nm->obstacle_count - 1; ++i)
        nm->obstacles[i] = nm->obstacles[i + 1];
    nm->obstacle_count--;
    if (nm->tile_size > 0.0 && nm->qgrid_nx > 0) {
        navmesh3d_reflag_obstacle_tiles(nm, removed.min, removed.max);
        return 1;
    }
    if (!navmesh3d_apply_slope_filter(nm)) {
        for (int32_t i = nm->obstacle_count; i > remove_at; --i)
            nm->obstacles[i] = nm->obstacles[i - 1];
        nm->obstacles[remove_at] = removed;
        nm->obstacle_count++;
        return 0;
    }
    return 1;
}

/// @brief Update a coarse AABB obstacle and immediately refilter the walkable triangle set.
/// @param obj NavMesh3D handle to modify.
/// @param index Zero-based obstacle index.
/// @param min_v First Vec3 corner of the replacement bounds.
/// @param max_v Opposite Vec3 corner of the replacement bounds.
/// @return One when the obstacle and affected mesh state are updated, otherwise zero.
int8_t rt_navmesh3d_update_obstacle(void *obj, int64_t index, void *min_v, void *max_v) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    float omin[3];
    float omax[3];
    nav_obstacle_t old_obstacle;
    int32_t obstacle_count = navmesh3d_read_obstacle_count(nm);
    if (!navmesh3d_can_edit_obstacles(nm) ||
        !navmesh3d_array_storage_valid(nm->obstacles, nm->obstacle_count, nm->obstacle_capacity) ||
        index < 0 || index >= obstacle_count)
        return 0;
    if (!navmesh3d_read_obstacle_bounds(min_v, max_v, omin, omax))
        return 0;
    int32_t edit_at = (int32_t)index;
    old_obstacle = nm->obstacles[edit_at];
    memcpy(nm->obstacles[edit_at].min, omin, sizeof(omin));
    memcpy(nm->obstacles[edit_at].max, omax, sizeof(omax));
    if (nm->tile_size > 0.0 && nm->qgrid_nx > 0) {
        navmesh3d_reflag_obstacle_tiles(nm, old_obstacle.min, old_obstacle.max);
        navmesh3d_reflag_obstacle_tiles(nm, omin, omax);
        return 1;
    }
    if (!navmesh3d_apply_slope_filter(nm)) {
        nm->obstacles[edit_at] = old_obstacle;
        return 0;
    }
    return 1;
}

/// @brief Number of authored coarse AABB obstacles.
/// @param obj NavMesh3D handle to inspect.
/// @return Non-negative obstacle count, or zero for an invalid handle.
int64_t rt_navmesh3d_get_obstacle_count(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    return navmesh3d_read_obstacle_count(nm);
}

/// @brief Assign nav-area and traversal-cost metadata to polygons overlapping an AABB volume.
/// @param obj NavMesh3D handle to modify.
/// @param min_v First Vec3 corner of the selection bounds.
/// @param max_v Opposite Vec3 corner of the selection bounds.
/// @param area Runtime area-name string to intern and assign.
/// @param traversal_cost Cost multiplier sanitized before assignment.
/// @return One when at least one preserved source triangle overlaps the bounds, otherwise zero.
int8_t rt_navmesh3d_set_area(
    void *obj, void *min_v, void *max_v, rt_string area, double traversal_cost) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm || !nm->source_triangles || !nm->triangles || !area || !rt_string_is_handle(area))
        return 0;
    float min[3];
    float max[3];
    if (!navmesh3d_read_obstacle_bounds(min_v, max_v, min, max))
        return 0;
    int32_t area_id = navmesh3d_ensure_area_id(nm, area);
    if (area_id < 0)
        return 0;
    float cost = navmesh3d_sanitize_traversal_cost(traversal_cost);
    int32_t touched = 0;
    for (int32_t i = 0; i < nm->source_triangle_count; i++) {
        nav_triangle_t *tri = &nm->source_triangles[i];
        if (!navmesh3d_triangle_overlaps_bounds(nm, tri, min, max))
            continue;
        tri->area_id = area_id;
        tri->traversal_cost = cost;
        touched++;
    }
    for (int32_t i = 0; i < nm->triangle_count; i++) {
        nav_triangle_t *tri = &nm->triangles[i];
        if (!navmesh3d_triangle_overlaps_bounds(nm, tri, min, max))
            continue;
        tri->area_id = area_id;
        tri->traversal_cost = cost;
    }
    return touched > 0 ? 1 : 0;
}

/// @brief Read the nav-area name metadata at a walkable position.
/// @param obj NavMesh3D handle to query.
/// @param point Vec3 world-space position to resolve.
/// @return Runtime area-name string for the containing triangle, or an empty constant string when
/// no walkable triangle is found.
rt_string rt_navmesh3d_get_area(void *obj, void *point) {
    float query[3];
    if (!obj || !navmesh3d_read_point3f(point, query))
        return rt_const_cstr("");
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm || !nm->triangles || !nm->vertices)
        return rt_const_cstr("");
    int32_t tri =
        find_tri_with_max_dy(nm, query[0], query[1], query[2], navmesh3d_vertical_tolerance(nm));
    if (tri < 0)
        return rt_const_cstr("");
    return navmesh3d_area_name(nm, nm->triangles[tri].area_id);
}

/// @brief Sample the navmesh traversal cost at world @p point — resolving the triangle directly
///   at/below it (via vertical proximity, so stacked surfaces disambiguate) and returning that
///   triangle's area-weighted cost. Returns 0 when @p point is off-mesh, non-finite, or invalid.
/// @param obj NavMesh3D handle to query.
/// @param point Vec3 world-space position to resolve.
/// @return Sanitized traversal cost for the containing triangle, or zero when none is found.
double rt_navmesh3d_get_traversal_cost(void *obj, void *point) {
    float query[3];
    if (!obj || !navmesh3d_read_point3f(point, query))
        return 0.0;
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm || !nm->triangles || !nm->vertices)
        return 0.0;
    int32_t tri =
        find_tri_with_max_dy(nm, query[0], query[1], query[2], navmesh3d_vertical_tolerance(nm));
    if (tri < 0)
        return 0.0;
    return (double)navmesh3d_tri_cost(nm, tri);
}

/// @brief Number of authored off-mesh traversal links.
/// @param obj NavMesh3D handle to inspect.
/// @return Non-negative off-mesh link count, or zero for an invalid handle.
int64_t rt_navmesh3d_get_offmesh_link_count(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    return navmesh3d_read_offmesh_link_count(nm);
}

/// @brief `NavMesh3D.TileSize` — the tile edge length used by O(tile) rebuilds.
/// @details Every `Bake` now auto-tiles (extent/16, clamped); `BakeTiled`
///          overrides explicitly. `Build`-constructed meshes report 0 (no
///          voxel source to re-carve; obstacle edits refilter the mesh).
/// @param obj NavMesh3D handle to inspect.
/// @return Positive tile edge length for a tiled mesh, otherwise zero.
double rt_navmesh3d_get_tile_size(void *obj) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    return nm && nm->tile_size > 0.0 ? nm->tile_size : 0.0;
}

/// @brief Does the segment (a -> b) correspond to a registered off-mesh link?
/// @details Matches either link direction for bidirectional links using a
///          0.25-unit endpoint tolerance (path corners are inserted at the
///          exact link endpoints, so the tolerance only absorbs float
///          round-trips). Writes the link's authored kind string (borrowed,
///          may be NULL) to @p out_kind when non-NULL.
/// @param obj NavMesh3D handle to query.
/// @param a Three-component world-space segment start.
/// @param b Three-component world-space segment end.
/// @param out_kind Optional output receiving the matched link's borrowed kind string.
/// @return One when the segment matches a permitted link direction, otherwise zero.
int8_t rt_navmesh3d_segment_is_offmesh_link(void *obj,
                                            const double *a,
                                            const double *b,
                                            rt_string *out_kind) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (out_kind)
        *out_kind = NULL;
    int32_t link_count = navmesh3d_read_offmesh_link_count(nm);
    if (!nm || !a || !b || link_count <= 0)
        return 0;
    const double tol2 = 0.25 * 0.25;
    for (int32_t i = 0; i < link_count; i++) {
        const nav_offmesh_link_t *link = &nm->offmesh_links[i];
        double f[3] = {link->from[0], link->from[1], link->from[2]};
        double t[3] = {link->to[0], link->to[1], link->to[2]};
        double dfa = (f[0] - a[0]) * (f[0] - a[0]) + (f[1] - a[1]) * (f[1] - a[1]) +
                     (f[2] - a[2]) * (f[2] - a[2]);
        double dtb = (t[0] - b[0]) * (t[0] - b[0]) + (t[1] - b[1]) * (t[1] - b[1]) +
                     (t[2] - b[2]) * (t[2] - b[2]);
        int forward = dfa <= tol2 && dtb <= tol2;
        int backward = 0;
        if (!forward && link->bidirectional) {
            double dfb = (f[0] - b[0]) * (f[0] - b[0]) + (f[1] - b[1]) * (f[1] - b[1]) +
                         (f[2] - b[2]) * (f[2] - b[2]);
            double dta = (t[0] - a[0]) * (t[0] - a[0]) + (t[1] - a[1]) * (t[1] - a[1]) +
                         (t[2] - a[2]) * (t[2] - a[2]);
            backward = dfb <= tol2 && dta <= tol2;
        }
        if (forward || backward) {
            if (out_kind)
                *out_kind = link->kind;
            return 1;
        }
    }
    return 0;
}

/// @brief Rebuild one tile after dynamic changes.
/// @details Real tiled data ownership is future work; this conservative baseline refilters the
/// preserved source triangles and rebuilds adjacency/off-mesh endpoint resolution for the whole
/// navmesh. The tile coordinates are accepted for API stability.
/// @param obj NavMesh3D handle to rebuild.
/// @param tile_x Signed tile X coordinate.
/// @param tile_z Signed tile Z coordinate.
/// @return One when the requested rebuild or refilter is accepted, otherwise zero.
int8_t rt_navmesh3d_rebuild_tile(void *obj, int64_t tile_x, int64_t tile_z) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm || !nm->triangles || nm->triangle_count <= 0)
        return 0;
    if (!navmesh3d_tile_index_valid(tile_x) || !navmesh3d_tile_index_valid(tile_z))
        return 0;
    /* Tiled voxel bake: rebuild just this tile's retained voxel source in place (O(tile)) — no
     * whole-scene voxel pass, adjacency rebuild, or query-grid rebuild. */
    if (nm->tile_size > 0.0 && nm->qgrid_nx > 0) {
        if (navmesh3d_has_voxel_source(nm))
            navmesh3d_rebuild_voxel_tile(nm, tile_x, tile_z);
        else {
            navmesh3d_reflag_tile(nm, tile_x, tile_z);
            navmesh3d_refresh_offmesh_links(nm);
        }
        return 1;
    }
    /* Non-tiled mesh (Build / untiled bake): preserve the whole-mesh refilter baseline. */
    if (!nm->source_triangles || nm->source_triangle_count <= 0)
        return 0;
    (void)tile_x;
    (void)tile_z;
    return navmesh3d_apply_slope_filter(nm) ? 1 : 0;
}

/// @brief Test-only: append an obstacle WITHOUT re-flagging any triangle.
/// @details Used by unit tests to create a "stale" tiled navmesh — the obstacle exists but no tile
/// has been re-carved yet — so a subsequent `RebuildTile` can be observed to affect exactly one
/// tile. Not part of the scripting surface (classified internal in RuntimeSurfacePolicy.inc).
/// @param obj NavMesh3D handle to modify.
/// @param min_v First Vec3 corner of the injected obstacle.
/// @param max_v Opposite Vec3 corner of the injected obstacle.
/// @return One when the obstacle is appended without applying it, otherwise zero.
int8_t rt_navmesh3d_test_inject_obstacle(void *obj, void *min_v, void *max_v) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!navmesh3d_can_edit_obstacles(nm))
        return 0;
    float omin[3];
    float omax[3];
    if (!navmesh3d_read_obstacle_bounds(min_v, max_v, omin, omax))
        return 0;
    if (!navmesh3d_array_storage_valid(nm->obstacles, nm->obstacle_count, nm->obstacle_capacity) ||
        nm->obstacle_count == INT32_MAX)
        return 0;
    if (nm->obstacle_count >= nm->obstacle_capacity) {
        int32_t new_cap = 4;
        if (nm->obstacle_capacity > 0) {
            if (nm->obstacle_capacity > INT32_MAX / 2)
                return 0;
            new_cap = nm->obstacle_capacity * 2;
        }
        if ((size_t)new_cap > SIZE_MAX / sizeof(nav_obstacle_t))
            return 0;
        nav_obstacle_t *obstacles =
            (nav_obstacle_t *)realloc(nm->obstacles, (size_t)new_cap * sizeof(nav_obstacle_t));
        if (!obstacles)
            return 0;
        nm->obstacles = obstacles;
        nm->obstacle_capacity = new_cap;
    }
    nav_obstacle_t *obstacle = &nm->obstacles[nm->obstacle_count++];
    memcpy(obstacle->min, omin, sizeof(omin));
    memcpy(obstacle->max, omax, sizeof(omax));
    return 1;
}

/// @brief Test-only: map a world XZ position to its tile coords (see navmesh3d_tile_of_point).
/// Returns 0 if the mesh is not tiled. Not part of the scripting surface.
/// @param obj NavMesh3D handle to query.
/// @param px Finite world-space X coordinate.
/// @param pz Finite world-space Z coordinate.
/// @param out_tx Receives the containing tile X coordinate.
/// @param out_tz Receives the containing tile Z coordinate.
/// @return One when the point maps to a tiled mesh, otherwise zero.
int8_t rt_navmesh3d_test_tile_of_point(
    void *obj, double px, double pz, int64_t *out_tx, int64_t *out_tz) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!isfinite(px) || !isfinite(pz))
        return 0;
    return (
        int8_t)(navmesh3d_tile_of_point(
                    nm, navmesh3d_coord_or(px, 0.0), navmesh3d_coord_or(pz, 0.0), out_tx, out_tz)
                    ? 1
                    : 0);
}

/// @brief Test-only: edit retained voxel source for a tile without touching the live mesh.
/// @details This simulates a tile-local geometry source change. A far-tile RebuildTile must leave
/// the live mesh stale; rebuilding this tile must refresh its geometry/blocking from the retained
/// source. Not part of the scripting surface.
/// @param obj NavMesh3D handle whose retained voxel source is edited.
/// @param tile_x Signed tile X coordinate.
/// @param tile_z Signed tile Z coordinate.
/// @param height Finite replacement height written to every voxel cell in the tile.
/// @param walkable Nonzero to mark the edited cells walkable.
/// @return One when the tile maps to retained voxel cells and the edit is applied, otherwise zero.
int8_t rt_navmesh3d_test_set_tile_source(
    void *obj, int64_t tile_x, int64_t tile_z, double height, int8_t walkable) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    int32_t cx0, cx1, cz0, cz1;
    if (!navmesh3d_voxel_tile_cell_range(nm, tile_x, tile_z, &cx0, &cx1, &cz0, &cz1) ||
        !isfinite(height))
        return 0;
    height = navmesh3d_coord_or(height, 0.0);
    for (int32_t cz = cz0; cz <= cz1; cz++) {
        for (int32_t cx = cx0; cx <= cx1; cx++) {
            size_t cell = navmesh3d_voxel_cell_index(nm, cx, cz);
            if (cell == SIZE_MAX)
                continue;
            nm->voxel_cell_height[cell] = (float)height;
            nm->voxel_cell_walkable[cell] = walkable ? 1 : 0;
        }
    }
    return 1;
}

/// @brief Set the maximum slope angle (in degrees) considered walkable.
/// Refilters the preserved source triangles and rebuilds adjacency immediately.
/// @param obj NavMesh3D handle to modify.
/// @param degrees Requested maximum walkable slope, sanitized to the supported range.
void rt_navmesh3d_set_max_slope(void *obj, double degrees) {
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm)
        return;
    double previous_slope = nm->max_slope;
    nm->max_slope = navmesh3d_sanitize_slope(degrees);
    if (!navmesh3d_apply_slope_filter(nm))
        nm->max_slope = previous_slope;
}

/// @brief Render the navmesh as wireframe / outlined triangles to a Canvas3D for debugging.
/// Useful for visualizing what's actually walkable vs. blocked.
/// @param obj NavMesh3D handle to visualize.
/// @param canvas Canvas3D handle receiving green triangle-edge line primitives.
void rt_navmesh3d_debug_draw(void *obj, void *canvas) {
    if (!obj || !canvas)
        return;
    rt_navmesh3d *nm = (rt_navmesh3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVMESH3D_CLASS_ID);
    if (!nm || nm->triangle_count <= 0 || !nm->vertices || !nm->triangles)
        return;
    int64_t color = 0x00FF44; /* green */

    for (int32_t i = 0; i < nm->triangle_count; i++) {
        nav_triangle_t *tri = &nm->triangles[i];
        if (tri->blocked)
            continue; /* carved out — not part of the walkable surface */
        float *v0 = nm->vertices[tri->v[0]].position;
        float *v1 = nm->vertices[tri->v[1]].position;
        float *v2 = nm->vertices[tri->v[2]].position;
        if (!navmesh3d_float3_finite(v0) || !navmesh3d_float3_finite(v1) ||
            !navmesh3d_float3_finite(v2))
            continue;
        /* Offset slightly above surface to avoid z-fighting */
        double off = 0.02;
        void *a = rt_vec3_new(v0[0], v0[1] + off, v0[2]);
        void *b = rt_vec3_new(v1[0], v1[1] + off, v1[2]);
        void *c = rt_vec3_new(v2[0], v2[1] + off, v2[2]);
        rt_canvas3d_draw_line3d(canvas, a, b, color);
        rt_canvas3d_draw_line3d(canvas, b, c, color);
        rt_canvas3d_draw_line3d(canvas, c, a, color);
        navmesh3d_release_local(a);
        navmesh3d_release_local(b);
        navmesh3d_release_local(c);
    }
}
