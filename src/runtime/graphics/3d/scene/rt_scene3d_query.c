//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_query.c
// Purpose: Scene3D spatial queries — AABB / sphere overlap and node raycast.
//   Each uses the BVH spatial index when available and falls back to an
//   iterative subtree walk otherwise. Split out of rt_scene3d.c; shares private
//   structs/helpers via rt_scene3d_internal.h.
// Links: rt_scene3d_internal.h, rt_seq.h, rt_vec3.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Scene3D AABB, sphere, and node-ray spatial queries.
/// @details Queries prefer the scene's refreshable BVH and reuse its pooled
///   candidate allocation. If indexing is disabled or unavailable, they perform
///   an iterative visible-subtree walk with reusable scene-owned stack storage.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d_internal.h"
#include "rt_object.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>

static int8_t g_scene3d_test_force_precise_hit_growth_failure = 0;

/// @brief Test-only deterministic allocation failure for precise collect-all growth.
void rt_scene3d_test_set_precise_hit_growth_failure(int8_t enabled) {
    g_scene3d_test_force_precise_hit_growth_failure = enabled ? 1 : 0;
}

/// @brief Borrow the scene's pooled query-candidate buffer, transferring ownership to the
///   returned list and clearing the scene's slot (avoids per-query allocation). Returns an
///   empty list when @p scene is NULL.
/// @param scene Borrowed scene whose native candidate allocation is transferred.
/// @return Candidate-list value owning the borrowed allocation, or an empty list.
static scene3d_spatial_candidate_list_t scene3d_query_borrow_candidates(rt_scene3d *scene) {
    scene3d_spatial_candidate_list_t candidates = {0};
    if (!scene)
        return candidates;
    candidates.items = scene->query_candidates;
    candidates.capacity = scene->query_candidate_capacity;
    scene->query_candidates = NULL;
    scene->query_candidate_capacity = 0;
    return candidates;
}

/// @brief Return a borrowed candidate buffer to the scene's pool for reuse (freeing whatever
///   was previously pooled), or free it outright when @p scene is NULL; resets @p candidates
///   to empty either way.
/// @param scene Borrowed scene receiving the allocation, or `NULL` to free it.
/// @param candidates Caller-owned candidate-list value reset after transfer/release.
static void scene3d_query_return_candidates(rt_scene3d *scene,
                                            scene3d_spatial_candidate_list_t *candidates) {
    if (!candidates)
        return;
    candidates->count = 0;
    if (scene) {
        free(scene->query_candidates);
        scene->query_candidates = candidates->items;
        scene->query_candidate_capacity = candidates->capacity;
    } else {
        free(candidates->items);
    }
    candidates->items = NULL;
    candidates->capacity = 0;
}

/// @brief `Scene3D.QueryAABB`: indexed when available, flat-walk fallback otherwise.
/// @param obj Borrowed Scene3D handle.
/// @param min_obj Borrowed Vec3 containing either query corner.
/// @param max_obj Borrowed Vec3 containing the opposite query corner.
/// @return New owned sequence of borrowed visible mesh-node handles in scene traversal order.
void *rt_scene3d_query_aabb(void *obj, void *min_obj, void *max_obj) {
    rt_scene3d *s = scene3d_checked(obj);
    void *result;
    size_t count = 0;
    double a[3];
    double b[3];
    double query_min[3];
    double query_max[3];
    if (!s || !s->root)
        return rt_seq_new_owned();
    if (!scene3d_read_vec3d(min_obj, a, "Scene3D.QueryAABB: min must be Vec3") ||
        !scene3d_read_vec3d(max_obj, b, "Scene3D.QueryAABB: max must be Vec3"))
        return rt_seq_new_owned();
    result = rt_seq_new_owned();
    if (!result)
        return NULL;
    for (int i = 0; i < 3; ++i) {
        query_min[i] = fmin(a[i], b[i]);
        query_max[i] = fmax(a[i], b[i]);
    }
    scene3d_canonicalize_aabb_d(query_min, query_max);
    if (s->use_spatial_index) {
        scene3d_spatial_candidate_list_t candidates = scene3d_query_borrow_candidates(s);
        if (scene3d_spatial_collect_aabb(s, query_min, query_max, &candidates, 0)) {
            for (int32_t i = 0; i < candidates.count; ++i) {
                rt_scene_node3d *current = candidates.items[i]->node;
                double world_min[3];
                double world_max[3];
                if (scene3d_node_world_mesh_aabb(current, world_min, world_max) &&
                    scene3d_aabb_intersects_aabb(world_min, world_max, query_min, query_max))
                    rt_seq_push(result, current);
            }
            scene3d_query_return_candidates(s, &candidates);
            return result;
        }
        scene3d_query_return_candidates(s, &candidates);
    }
    if (!scene_node_stack_push(
            &s->query_traversal_stack, &count, &s->query_traversal_stack_capacity, s->root)) {
        rt_trap("Scene3D.QueryAABB: traversal stack allocation failed");
        return result;
    }
    while (count > 0) {
        rt_scene_node3d *current = s->query_traversal_stack[--count];
        double world_min[3];
        double world_max[3];
        if (!current->visible)
            continue;
        if (scene3d_node_world_mesh_aabb(current, world_min, world_max) &&
            scene3d_aabb_intersects_aabb(world_min, world_max, query_min, query_max))
            rt_seq_push(result, current);
        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; --i) {
            if (!scene_node_stack_push(&s->query_traversal_stack,
                                       &count,
                                       &s->query_traversal_stack_capacity,
                                       current->children[i])) {
                rt_trap("Scene3D.QueryAABB: traversal stack allocation failed");
                return result;
            }
        }
    }
    return result;
}

/// @brief `Scene3D.QuerySphere`: indexed when available, flat-walk fallback otherwise.
/// @param obj Borrowed Scene3D handle.
/// @param center_obj Borrowed Vec3 containing the sphere center.
/// @param radius Candidate radius sanitized to a non-negative scene distance.
/// @return New owned sequence of borrowed visible mesh-node handles in scene traversal order.
void *rt_scene3d_query_sphere(void *obj, void *center_obj, double radius) {
    rt_scene3d *s = scene3d_checked(obj);
    void *result;
    size_t count = 0;
    double center[3];
    double r;
    if (!s || !s->root)
        return rt_seq_new_owned();
    if (!scene3d_read_vec3d(center_obj, center, "Scene3D.QuerySphere: center must be Vec3"))
        return rt_seq_new_owned();
    result = rt_seq_new_owned();
    if (!result)
        return NULL;
    r = scene3d_distance_or_zero(radius);
    if (s->use_spatial_index) {
        scene3d_spatial_candidate_list_t candidates = scene3d_query_borrow_candidates(s);
        double query_min[3] = {scene3d_clamp_abs_or(center[0] - r, 0.0),
                               scene3d_clamp_abs_or(center[1] - r, 0.0),
                               scene3d_clamp_abs_or(center[2] - r, 0.0)};
        double query_max[3] = {scene3d_clamp_abs_or(center[0] + r, 0.0),
                               scene3d_clamp_abs_or(center[1] + r, 0.0),
                               scene3d_clamp_abs_or(center[2] + r, 0.0)};
        scene3d_canonicalize_aabb_d(query_min, query_max);
        if (scene3d_spatial_collect_aabb(s, query_min, query_max, &candidates, 0)) {
            for (int32_t i = 0; i < candidates.count; ++i) {
                rt_scene_node3d *current = candidates.items[i]->node;
                double world_min[3];
                double world_max[3];
                if (scene3d_node_world_mesh_aabb(current, world_min, world_max) &&
                    scene3d_aabb_intersects_sphere(world_min, world_max, center, r))
                    rt_seq_push(result, current);
            }
            scene3d_query_return_candidates(s, &candidates);
            return result;
        }
        scene3d_query_return_candidates(s, &candidates);
    }
    if (!scene_node_stack_push(
            &s->query_traversal_stack, &count, &s->query_traversal_stack_capacity, s->root)) {
        rt_trap("Scene3D.QuerySphere: traversal stack allocation failed");
        return result;
    }
    while (count > 0) {
        rt_scene_node3d *current = s->query_traversal_stack[--count];
        double world_min[3];
        double world_max[3];
        if (!current->visible)
            continue;
        if (scene3d_node_world_mesh_aabb(current, world_min, world_max) &&
            scene3d_aabb_intersects_sphere(world_min, world_max, center, r))
            rt_seq_push(result, current);
        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; --i) {
            if (!scene_node_stack_push(&s->query_traversal_stack,
                                       &count,
                                       &s->query_traversal_stack_capacity,
                                       current->children[i])) {
                rt_trap("Scene3D.QuerySphere: traversal stack allocation failed");
                return result;
            }
        }
    }
    return result;
}

/// @brief Return the closest visible mesh node whose world AABB intersects the ray.
/// @details The direction is normalized internally. Ties select the later candidate
///   in stable traversal order because equal entry distances replace the current hit.
/// @param obj Borrowed Scene3D handle.
/// @param origin_obj Borrowed Vec3 containing the ray origin.
/// @param direction_obj Borrowed non-degenerate Vec3 direction.
/// @param max_distance Maximum non-negative hit distance; non-finite input uses the scene maximum.
/// @return Borrowed closest visible mesh node, or `NULL` when no valid hit exists.
void *rt_scene3d_raycast_nodes(void *obj,
                               void *origin_obj,
                               void *direction_obj,
                               double max_distance) {
    rt_scene3d *s = scene3d_checked(obj);
    size_t count = 0;
    double origin[3];
    double direction[3];
    double best_t;
    rt_scene_node3d *best = NULL;
    if (!s || !s->root)
        return NULL;
    if (!scene3d_read_vec3d(origin_obj, origin, "Scene3D.RaycastNodes: origin must be Vec3") ||
        !scene3d_read_vec3d(
            direction_obj, direction, "Scene3D.RaycastNodes: direction must be Vec3"))
        return NULL;
    if (!scene3d_normalize_vec3d(direction))
        return NULL;
    if (!isfinite(max_distance))
        max_distance = SCENE3D_ABS_MAX;
    if (max_distance < 0.0)
        return NULL;
    max_distance = scene3d_distance_or_zero(max_distance);
    best_t = max_distance;
    if (s->use_spatial_index) {
        scene3d_spatial_candidate_list_t candidates = scene3d_query_borrow_candidates(s);
        double query_min[3];
        double query_max[3];
        int ok;
        if (scene3d_ray_sweep_bounds(origin, direction, max_distance, query_min, query_max))
            ok = scene3d_spatial_collect_aabb(s, query_min, query_max, &candidates, 0);
        else
            ok = scene3d_spatial_collect_all(s, &candidates);
        if (ok) {
            for (int32_t i = 0; i < candidates.count; ++i) {
                rt_scene_node3d *current = candidates.items[i]->node;
                double world_min[3];
                double world_max[3];
                double t;
                if (scene3d_node_world_mesh_aabb(current, world_min, world_max) &&
                    scene3d_ray_intersects_aabb(
                        origin, direction, world_min, world_max, best_t, &t)) {
                    if (t <= best_t) {
                        best_t = t;
                        best = current;
                    }
                }
            }
            scene3d_query_return_candidates(s, &candidates);
            return best;
        }
        scene3d_query_return_candidates(s, &candidates);
    }
    if (!scene_node_stack_push(
            &s->query_traversal_stack, &count, &s->query_traversal_stack_capacity, s->root)) {
        rt_trap("Scene3D.RaycastNodes: traversal stack allocation failed");
        return NULL;
    }
    while (count > 0) {
        rt_scene_node3d *current = s->query_traversal_stack[--count];
        double world_min[3];
        double world_max[3];
        double t;
        if (!current->visible)
            continue;
        if (scene3d_node_world_mesh_aabb(current, world_min, world_max) &&
            scene3d_ray_intersects_aabb(origin, direction, world_min, world_max, best_t, &t)) {
            if (t <= best_t) {
                best_t = t;
                best = current;
            }
        }
        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; --i) {
            if (!scene_node_stack_push(&s->query_traversal_stack,
                                       &count,
                                       &s->query_traversal_stack_capacity,
                                       current->children[i])) {
                rt_trap("Scene3D.RaycastNodes: traversal stack allocation failed");
                return best;
            }
        }
    }
    return best;
}

/*==========================================================================
 * Triangle-accurate raycast queries (ADR 0193)
 *=========================================================================*/

/// @brief One triangle-accurate node hit produced by the precise raycast walk.
typedef struct {
    /// Borrowed hit node.
    rt_scene_node3d *node;
    /// Euclidean world hit distance along the normalized ray.
    double distance;
    /// Winning triangle index within the node's mesh.
    int64_t triangle;
    /// World-space hit point.
    double point[3];
    /// Normalized world-space face normal.
    double normal[3];
    /// Stable traversal order used to keep distance ties deterministic.
    int64_t order;
} scene3d_precise_hit_t;

/// @brief Accumulator threaded through the precise raycast traversal.
typedef struct {
    /// Nonzero to collect every hit node; zero keeps only the nearest.
    int collect_all;
    /// Query distance cap in world units.
    double max_distance;
    /// Nearest hit; `best.node` is `NULL` before the first hit.
    scene3d_precise_hit_t best;
    /// Owned hit array used when @ref collect_all is set.
    scene3d_precise_hit_t *items;
    /// Number of valid entries in @ref items.
    int32_t count;
    /// Allocated capacity of @ref items.
    int32_t capacity;
    /// Monotonic traversal counter stamped onto each accepted hit.
    int64_t order_counter;
} scene3d_precise_acc_t;

/// @brief AABB-prefiltered triangle test for one candidate node.
/// @details `scene3d_node_world_mesh_aabb` refreshes the node's world matrix,
///   so the raw mesh query always sees current transforms. The AABB entry
///   distance gates the narrow phase against @p cap because a box entry
///   beyond the cap cannot contain a closer triangle.
/// @param node Borrowed candidate node.
/// @param origin Borrowed world ray origin.
/// @param direction Borrowed normalized world ray direction.
/// @param cap Maximum accepted Euclidean hit distance.
/// @param out Output receiving the hit record (order not stamped).
/// @return Nonzero when the node's mesh has a triangle hit within @p cap.
static int scene3d_precise_test_node(rt_scene_node3d *node,
                                     const double origin[3],
                                     const double direction[3],
                                     double cap,
                                     scene3d_precise_hit_t *out) {
    rt_mesh3d *mesh;
    double world_min[3];
    double world_max[3];
    double aabb_t;
    if (!node || !node->mesh || !out)
        return 0;
    if (!scene3d_node_world_mesh_aabb(node, world_min, world_max))
        return 0;
    if (!scene3d_ray_intersects_aabb(origin, direction, world_min, world_max, cap, &aabb_t))
        return 0;
    mesh = (rt_mesh3d *)rt_g3d_checked_or_null(node->mesh, RT_G3D_MESH3D_CLASS_ID);
    if (!mesh)
        return 0;
    if (!rt_raycast3d_mesh_hit_raw(origin,
                                   direction,
                                   mesh,
                                   node->world_matrix,
                                   cap,
                                   &out->distance,
                                   &out->triangle,
                                   out->point,
                                   out->normal))
        return 0;
    out->node = node;
    return 1;
}

/// @brief Fold one candidate node into the precise-raycast accumulator.
/// @details Nearest-only mode prunes with the best distance so far and lets
///   equal distances replace the current hit, matching the AABB query's
///   later-traversal tie rule. Collect-all mode records every hit node once.
/// @param acc Accumulator owning collection state.
/// @param node Borrowed candidate node.
/// @param origin Borrowed world ray origin.
/// @param direction Borrowed normalized world ray direction.
/// @return Nonzero on success; zero when the result array failed to grow.
static int scene3d_precise_consider_node(scene3d_precise_acc_t *acc,
                                         rt_scene_node3d *node,
                                         const double origin[3],
                                         const double direction[3]) {
    scene3d_precise_hit_t hit;
    int32_t next_capacity;
    if (!acc || acc->count < 0 || acc->capacity < 0 || acc->count > acc->capacity ||
        (acc->capacity > 0 && !acc->items))
        return 0;
    double cap = acc->max_distance;
    if (!acc->collect_all && acc->best.node)
        cap = acc->best.distance;
    if (!scene3d_precise_test_node(node, origin, direction, cap, &hit))
        return 1;
    hit.order = acc->order_counter++;
    if (!acc->collect_all) {
        acc->best = hit;
        return 1;
    }
    if (acc->count == acc->capacity) {
        if (acc->count == INT32_MAX)
            return 0;
        if (acc->capacity <= 0)
            next_capacity = 16;
        else if (acc->capacity > INT32_MAX / 2)
            next_capacity = acc->count + 1;
        else
            next_capacity = acc->capacity * 2;
        if (next_capacity <= acc->count || (size_t)next_capacity > SIZE_MAX / sizeof(*acc->items))
            return 0;
        if (g_scene3d_test_force_precise_hit_growth_failure)
            return 0;
        scene3d_precise_hit_t *grown = (scene3d_precise_hit_t *)realloc(
            acc->items, (size_t)next_capacity * sizeof(scene3d_precise_hit_t));
        if (!grown)
            return 0;
        acc->items = grown;
        acc->capacity = next_capacity;
    }
    acc->items[acc->count++] = hit;
    if (!acc->best.node || hit.distance <= acc->best.distance)
        acc->best = hit;
    return 1;
}

/// @brief Compare precise hits by distance, then stable traversal order.
/// @param lhs Borrowed left hit record.
/// @param rhs Borrowed right hit record.
/// @return Standard qsort ordering result.
static int scene3d_precise_hit_compare(const void *lhs, const void *rhs) {
    const scene3d_precise_hit_t *a = (const scene3d_precise_hit_t *)lhs;
    const scene3d_precise_hit_t *b = (const scene3d_precise_hit_t *)rhs;
    if (a->distance < b->distance)
        return -1;
    if (a->distance > b->distance)
        return 1;
    if (a->order < b->order)
        return -1;
    if (a->order > b->order)
        return 1;
    return 0;
}

/// @brief Shared traversal for the triangle-accurate raycast queries.
/// @details Mirrors `rt_scene3d_raycast_nodes`: the spatial index supplies
///   candidates from the ray's swept bounds when enabled, and an iterative
///   visible-subtree walk covers every other case. Ownership of `acc->items`
///   stays with the caller regardless of outcome.
/// @param s Borrowed validated scene.
/// @param origin_obj Borrowed Vec3 ray origin handle.
/// @param direction_obj Borrowed Vec3 ray direction handle.
/// @param max_distance Requested hit cap; non-finite means the scene maximum.
/// @param trap_message Message used when traversal stack allocation fails.
/// @param acc Accumulator configured by the caller.
/// @return Nonzero when the walk ran (even if nothing was hit).
static int scene3d_raycast_precise_walk(rt_scene3d *s,
                                        void *origin_obj,
                                        void *direction_obj,
                                        double max_distance,
                                        const char *trap_message,
                                        scene3d_precise_acc_t *acc) {
    size_t count = 0;
    double origin[3];
    double direction[3];
    if (!s || !s->root)
        return 0;
    if (!scene3d_read_vec3d(
            origin_obj, origin, "Scene3D.RaycastNodesPrecise: origin must be Vec3") ||
        !scene3d_read_vec3d(
            direction_obj, direction, "Scene3D.RaycastNodesPrecise: direction must be Vec3"))
        return 0;
    if (!scene3d_normalize_vec3d(direction))
        return 0;
    if (!isfinite(max_distance))
        max_distance = SCENE3D_ABS_MAX;
    if (max_distance < 0.0)
        return 0;
    acc->max_distance = scene3d_distance_or_zero(max_distance);
    if (s->use_spatial_index) {
        scene3d_spatial_candidate_list_t candidates = scene3d_query_borrow_candidates(s);
        double query_min[3];
        double query_max[3];
        int ok;
        if (scene3d_ray_sweep_bounds(origin, direction, acc->max_distance, query_min, query_max))
            ok = scene3d_spatial_collect_aabb(s, query_min, query_max, &candidates, 0);
        else
            ok = scene3d_spatial_collect_all(s, &candidates);
        if (ok) {
            for (int32_t i = 0; i < candidates.count; ++i) {
                if (!scene3d_precise_consider_node(
                        acc, candidates.items[i]->node, origin, direction)) {
                    scene3d_query_return_candidates(s, &candidates);
                    rt_trap(trap_message);
                    return 0;
                }
            }
            scene3d_query_return_candidates(s, &candidates);
            return 1;
        }
        scene3d_query_return_candidates(s, &candidates);
    }
    if (!scene_node_stack_push(
            &s->query_traversal_stack, &count, &s->query_traversal_stack_capacity, s->root)) {
        rt_trap(trap_message);
        return 0;
    }
    while (count > 0) {
        rt_scene_node3d *current = s->query_traversal_stack[--count];
        if (!current->visible)
            continue;
        if (!scene3d_precise_consider_node(acc, current, origin, direction)) {
            rt_trap(trap_message);
            return 0;
        }
        for (int32_t i = scene3d_node_child_count(current) - 1; i >= 0; --i) {
            if (!scene_node_stack_push(&s->query_traversal_stack,
                                       &count,
                                       &s->query_traversal_stack_capacity,
                                       current->children[i])) {
                rt_trap(trap_message);
                return 0;
            }
        }
    }
    return 1;
}

/// @brief `Scene3D.RaycastNodesPrecise`: nearest triangle-accurate node hit.
/// @param obj Borrowed Scene3D handle.
/// @param origin_obj Borrowed Vec3 containing the ray origin.
/// @param direction_obj Borrowed non-degenerate Vec3 direction.
/// @param max_distance Maximum non-negative hit distance; non-finite input uses the scene maximum.
/// @return Borrowed visible mesh node containing the nearest intersected triangle, or `NULL`.
void *rt_scene3d_raycast_nodes_precise(void *obj,
                                       void *origin_obj,
                                       void *direction_obj,
                                       double max_distance) {
    rt_scene3d *s = scene3d_checked(obj);
    scene3d_precise_acc_t acc = {0};
    if (!scene3d_raycast_precise_walk(
            s,
            origin_obj,
            direction_obj,
            max_distance,
            "Scene3D.RaycastNodesPrecise: traversal stack allocation failed",
            &acc))
        return NULL;
    return acc.best.node;
}

/// @brief `Scene3D.RaycastNodesPreciseAll`: every triangle-hit node, nearest first.
/// @param obj Borrowed Scene3D handle.
/// @param origin_obj Borrowed Vec3 containing the ray origin.
/// @param direction_obj Borrowed non-degenerate Vec3 direction.
/// @param max_distance Maximum non-negative hit distance; non-finite input uses the scene maximum.
/// @return New owned sequence of borrowed hit nodes sorted by hit distance
///   (distance ties keep stable traversal order).
void *rt_scene3d_raycast_nodes_precise_all(void *obj,
                                           void *origin_obj,
                                           void *direction_obj,
                                           double max_distance) {
    rt_scene3d *s = scene3d_checked(obj);
    scene3d_precise_acc_t acc = {0};
    void *result;
    acc.collect_all = 1;
    if (!scene3d_raycast_precise_walk(s,
                                      origin_obj,
                                      direction_obj,
                                      max_distance,
                                      "Scene3D.RaycastNodesPreciseAll: result allocation failed",
                                      &acc)) {
        free(acc.items);
        return NULL;
    }
    result = rt_seq_new_owned();
    if (!result) {
        free(acc.items);
        return NULL;
    }
    if (acc.count > 1)
        qsort(acc.items,
              (size_t)acc.count,
              sizeof(scene3d_precise_hit_t),
              scene3d_precise_hit_compare);
    for (int32_t i = 0; i < acc.count; ++i)
        rt_seq_push(result, acc.items[i].node);
    free(acc.items);
    return result;
}

/// @brief `Scene3D.RaycastPreciseHit`: the nearest triangle hit itself.
/// @param obj Borrowed Scene3D handle.
/// @param origin_obj Borrowed Vec3 containing the ray origin.
/// @param direction_obj Borrowed non-degenerate Vec3 direction.
/// @param max_distance Maximum non-negative hit distance; non-finite input uses the scene maximum.
/// @return New RayHit3D carrying the world point, face normal, Euclidean
///   distance, and triangle index of the nearest hit, or `NULL` on a miss.
void *rt_scene3d_raycast_precise_hit(void *obj,
                                     void *origin_obj,
                                     void *direction_obj,
                                     double max_distance) {
    rt_scene3d *s = scene3d_checked(obj);
    scene3d_precise_acc_t acc = {0};
    if (!scene3d_raycast_precise_walk(
            s,
            origin_obj,
            direction_obj,
            max_distance,
            "Scene3D.RaycastPreciseHit: traversal stack allocation failed",
            &acc))
        return NULL;
    if (!acc.best.node)
        return NULL;
    return rt_raycast3d_build_hit(
        acc.best.distance, acc.best.point, acc.best.normal, acc.best.triangle);
}

#endif /* ZANNA_ENABLE_GRAPHICS */
