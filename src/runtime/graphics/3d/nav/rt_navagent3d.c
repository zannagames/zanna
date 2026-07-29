//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/nav/rt_navagent3d.c
// Purpose: Gameplay-facing single-agent navigation built on NavMesh3D path
//   queries, with optional Character3D and SceneNode3D bindings.
//
// Key invariants:
//   - Agent positions are snapped onto the bound NavMesh3D (closest valid point).
//   - Path corners are stored as a packed `[x0,y0,z0, x1,y1,z1, ...]` buffer.
//   - `path_index` advances past corners within `corner_tolerance` of the agent.
//   - When a CharacterController3D is bound it owns the authoritative position;
//     otherwise the agent integrates `desired_velocity * dt` directly.
//   - Auto-repath fires every `repath_interval` seconds while a target is active.
//   - Batch updates prepare all agents, snapshot all peers, solve immutably, and
//     apply in monotonic creation order, independent of caller array order.
//
// Ownership/Lifetime:
//   - NavAgent3D is GC-managed; finalizer frees the path buffer and releases the
//     navmesh / bound character / bound node refs.
//   - All bindings (navmesh, character, node) are retained on assignment and
//     released on rebind / finalize.
//   - Process-lifetime batch scratch borrows but never retains agent pointers.
//
// Links: rt_navagent3d.h, rt_navmesh3d.h, rt_physics3d.h, rt_scene3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_navagent3d.c
 * @brief Implements path following, bindings, and reciprocal local avoidance for NavAgent3D.
 *
 * Agents follow packed NavMesh3D corner paths, optionally synchronize with CharacterController3D
 * or SceneNode3D objects, and steer around peers through a deterministic sampled RVO solver. A
 * spatial hash accelerates individual updates, while batch updates snapshot all live agents and
 * publish results in stable creation order. Numeric inputs are bounded at the runtime boundary,
 * and all retained object references are released by the GC finalizer.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_navagent3d.h"

#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_mat4.h"
#include "rt_navmesh3d.h"
#include "rt_physics3d.h"
#include "rt_platform.h"
#include "rt_scene3d.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rt_string.h"

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);

typedef struct rt_navagent3d {
    void *vptr;
    void *navmesh;
    void *bound_character;
    void *bound_node;
    double radius;
    double height;
    double avoidance_radius;
    double position[3];
    double velocity[3];
    double desired_velocity[3];
    double target[3];
    double stopping_distance;
    double desired_speed;
    double remaining_distance;
    double repath_interval;
    double repath_accum;
    double *path_points_xyz;
    struct rt_navagent3d **avoidance_neighbors;
    int32_t path_point_count;
    int32_t path_index;
    int32_t avoidance_neighbor_capacity;
    int8_t has_target;
    int8_t has_path;
    int8_t auto_repath;
    int8_t avoidance_enabled;
    struct rt_navagent3d *registry_next;
    /* Uniform spatial-hash grid links for O(1)-ish neighbor queries during avoidance. The agent is
     * present in exactly one cell (grid_cx,grid_cz); the cell is refreshed whenever position syncs.
     */
    struct rt_navagent3d *grid_next;
    int32_t grid_cx;
    int32_t grid_cz;
    int8_t in_grid;
    uint64_t stable_order;
} rt_navagent3d;

static rt_navagent3d *g_navagent3d_registry = NULL;
/// @brief Live count of agents in g_navagent3d_registry, maintained alongside the linked
/// list. Lets the RVO neighbour search compare a grid scan's cost (~(2*ring+1)^2 cells)
/// against a full registry walk (this many agents) and pick the cheaper one, rather than
/// falling back to the registry at a fixed ring cap (which made one large-reach agent force
/// every agent onto the O(N) walk). Main-thread only, like the registry itself.
static int64_t g_navagent3d_registry_count = 0;

/* Spatial hash over agent XZ positions: each registered agent lives in exactly one bucket so
 * avoidance can scan a small cell neighborhood instead of the whole registry (O(N^2) -> ~O(N)). */
#define NAVAGENT_GRID_CELL 4.0
#define NAVAGENT_GRID_BUCKETS 1024u /* power of two for mask-based modulo */
#define NAVAGENT_GRID_MAX_RING 16   /* beyond this, fall back to a full registry scan */
#define NAVAGENT_RVO_MIN_TIME_HORIZON 0.65
#define NAVAGENT_RVO_MAX_TIME_HORIZON 2.0
#define NAVAGENT_RVO_MAX_CANDIDATES 48
#define NAVAGENT_COORD_ABS_MAX 1000000000000.0
#define NAVAGENT_SPEED_MAX 1000000.0
#define NAVAGENT_RADIUS_MAX 1000000.0
#define NAVAGENT_HEIGHT_MAX 1000000.0
#define NAVAGENT_DT_MAX 0.25
#define NAVAGENT_DISTANCE_MAX 1000000000000000000.0
/* Sentinel returned by the numeric getters for an invalid agent handle. Every valid result is
 * clamped to >= 0, so a negative value is an unambiguous "invalid query" signal that callers can
 * distinguish from a legitimate zero (e.g. "no path" / "at goal"). */
#define NAVAGENT_INVALID_QUERY (-1.0)
static rt_navagent3d *g_navagent3d_grid[NAVAGENT_GRID_BUCKETS];
/* Monotonic max of (effective avoidance radius + desired speed) across agents; bounds the cell
 * neighborhood a query must cover so the grid never misses a contributing peer. */
static double g_navagent3d_max_reach = 0.0;
/* Monotonic creation order used to canonicalize batch publication independently of the caller's
 * array order. Registry operations and agent construction are main-thread-only. */
static uint64_t g_navagent3d_next_stable_order = 0;

static void navagent_grid_refresh(rt_navagent3d *agent);
static void navagent_grid_remove(rt_navagent3d *agent);
static void navagent_recompute_max_reach(void);
static double navagent_clamp_abs_or(double value, double fallback, double max_abs);
static double navagent_coord_or(double value, double fallback);

/// @brief Validate @p obj as a NavAgent3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Opaque runtime object handle.
/// @return Typed agent pointer when @p obj has the NavAgent3D class, otherwise null.
static rt_navagent3d *navagent3d_checked(void *obj) {
    return (rt_navagent3d *)rt_g3d_checked_or_null(obj, RT_G3D_NAVAGENT3D_CLASS_ID);
}

/// @brief Add an agent to the global registry and insert it into the spatial grid at its cell.
/// @param agent Mutable newly created agent; null is a no-op. Must be called on the main thread.
static void navagent_register(rt_navagent3d *agent) {
    RT_ASSERT_MAIN_THREAD();
    if (!agent)
        return;
    agent->registry_next = g_navagent3d_registry;
    g_navagent3d_registry = agent;
    g_navagent3d_registry_count++;
    navagent_grid_refresh(agent); /* insert into the spatial grid at its current cell */
}

/// @brief Remove an agent from the spatial grid and unlink it from the global registry.
/// @param agent Mutable registered agent; null is a no-op. Must be called on the main thread.
static void navagent_unregister(rt_navagent3d *agent) {
    RT_ASSERT_MAIN_THREAD();
    rt_navagent3d **link = &g_navagent3d_registry;
    if (!agent)
        return;
    navagent_grid_remove(agent);
    while (*link) {
        if (*link == agent) {
            *link = agent->registry_next;
            agent->registry_next = NULL;
            if (g_navagent3d_registry_count > 0)
                g_navagent3d_registry_count--;
            navagent_recompute_max_reach();
            return;
        }
        link = &(*link)->registry_next;
    }
}

/// @brief Squared length of a 3-vector; avoids a sqrt when only ordering/thresholding matters.
/// @param v Optional borrowed vector; null and invalid lanes are treated as zero.
/// @return Finite squared length, saturated to @c DBL_MAX on overflow.
static double navagent_len_sq(const double v[3]) {
    double x = v ? navagent_coord_or(v[0], 0.0) : 0.0;
    double y = v ? navagent_coord_or(v[1], 0.0) : 0.0;
    double z = v ? navagent_coord_or(v[2], 0.0) : 0.0;
    double max_abs = fmax(fabs(x), fmax(fabs(y), fabs(z)));
    double len_sq;
    if (!isfinite(max_abs))
        return DBL_MAX;
    if (max_abs == 0.0)
        return 0.0;
    x /= max_abs;
    y /= max_abs;
    z /= max_abs;
    if (max_abs > sqrt(DBL_MAX / (x * x + y * y + z * z)))
        return DBL_MAX;
    len_sq = max_abs * max_abs * (x * x + y * y + z * z);
    return isfinite(len_sq) && len_sq >= 0.0 ? len_sq : DBL_MAX;
}

/// @brief Euclidean length of a 3-vector (`sqrt` of the squared length).
/// @param v Optional borrowed vector; null and invalid lanes are treated as zero.
/// @return Finite vector length, or zero when the scaled calculation is unusable.
static double navagent_len(const double v[3]) {
    double x = v ? navagent_coord_or(v[0], 0.0) : 0.0;
    double y = v ? navagent_coord_or(v[1], 0.0) : 0.0;
    double z = v ? navagent_coord_or(v[2], 0.0) : 0.0;
    double max_abs = fmax(fabs(x), fmax(fabs(y), fabs(z)));
    double len = 0.0;
    if (isfinite(max_abs) && max_abs > 0.0) {
        x /= max_abs;
        y /= max_abs;
        z /= max_abs;
        len = max_abs * sqrt(x * x + y * y + z * z);
    }
    return isfinite(len) ? len : 0.0;
}

/// @brief Clamp a finite scalar into ±max_abs, using fallback for NaN/Inf.
/// @param value Preferred value.
/// @param fallback Replacement for non-finite @p value; non-finite fallback becomes zero.
/// @param max_abs Requested finite non-negative absolute cap; invalid input disables the cap.
/// @return Sanitized value within the requested symmetric bound.
static double navagent_clamp_abs_or(double value, double fallback, double max_abs) {
    if (!isfinite(fallback))
        fallback = 0.0;
    if (!isfinite(max_abs) || max_abs < 0.0)
        max_abs = 0.0;
    if (!isfinite(value))
        value = fallback;
    if (max_abs > 0.0 && value > max_abs)
        return max_abs;
    if (max_abs > 0.0 && value < -max_abs)
        return -max_abs;
    return value;
}

/// @brief Sanitize one world-coordinate lane.
/// @param value Preferred coordinate.
/// @param fallback Replacement for non-finite input.
/// @return Finite coordinate clamped to the runtime world-coordinate bound.
static double navagent_coord_or(double value, double fallback) {
    return navagent_clamp_abs_or(value, fallback, NAVAGENT_COORD_ABS_MAX);
}

/// @brief Sanitize a non-negative scalar with an upper cap.
/// @param value Preferred scalar.
/// @param fallback Replacement for non-finite input.
/// @param max_value Optional finite positive upper bound.
/// @return Finite non-negative scalar constrained by @p max_value when usable.
static double navagent_nonnegative_capped_or(double value, double fallback, double max_value) {
    value = isfinite(value) ? value : fallback;
    if (!isfinite(value) || value < 0.0)
        value = 0.0;
    if (isfinite(max_value) && max_value > 0.0 && value > max_value)
        value = max_value;
    return value;
}

/// @brief Euclidean distance between two points.
/// @param a Optional borrowed first point; null and invalid lanes become zero.
/// @param b Optional borrowed second point; null and invalid lanes become zero.
/// @return Finite distance, or @c DBL_MAX when the difference is unrepresentable.
static double navagent_dist(const double a[3], const double b[3]) {
    double dx = navagent_coord_or(a ? a[0] : 0.0, 0.0) - navagent_coord_or(b ? b[0] : 0.0, 0.0);
    double dy = navagent_coord_or(a ? a[1] : 0.0, 0.0) - navagent_coord_or(b ? b[1] : 0.0, 0.0);
    double dz = navagent_coord_or(a ? a[2] : 0.0, 0.0) - navagent_coord_or(b ? b[2] : 0.0, 0.0);
    double delta[3] = {dx, dy, dz};
    double dist = navagent_len(delta);
    return isfinite(dist) ? dist : DBL_MAX;
}

/// @brief Assign `(x,y,z)` to `dst[0..2]`. Reads declaratively as "set this vector to …".
/// @param dst Caller-owned three-component destination.
/// @param x X coordinate to sanitize and store.
/// @param y Y coordinate to sanitize and store.
/// @param z Z coordinate to sanitize and store.
static void navagent_vec_set(double dst[3], double x, double y, double z) {
    dst[0] = navagent_coord_or(x, 0.0);
    dst[1] = navagent_coord_or(y, 0.0);
    dst[2] = navagent_coord_or(z, 0.0);
}

/// @brief Copy `src[0..2]` into `dst[0..2]` — trivial 3-double `memcpy`-equivalent.
/// @param dst Caller-owned three-component destination.
/// @param src Optional borrowed source; null stores the zero vector.
static void navagent_vec_copy(double dst[3], const double src[3]) {
    if (!src) {
        navagent_vec_set(dst, 0.0, 0.0, 0.0);
        return;
    }
    navagent_vec_set(dst, src[0], src[1], src[2]);
}

/// @brief Decrement-and-free a GC reference slot, clearing the slot to NULL.
/// @details Used when rebinding a navmesh / character / scene-node. The slot is zeroed
///   so subsequent calls are idempotent, and the finalizer can call this on every slot
///   without needing per-field null checks.
/// @param slot Mutable retained-reference slot to release and clear.
static void navagent_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release a retained Graphics3D binding only if it still has the expected class.
/// @param slot Mutable retained-reference slot. Mismatched objects are cleared without release.
/// @param class_id Required Graphics3D runtime class identifier.
static void navagent_release_class_ref(void **slot, int64_t class_id) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, class_id)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    navagent_release_ref(slot);
}

/// @brief Release a locally-held GC reference, freeing the object if the refcount reaches zero.
/// @details Used by navagent cleanup to drop borrowed pointers (mesh, path, etc.)
///          that were retained at assignment time.
/// @param obj Locally retained runtime object; null is a no-op.
static void navagent_release_local(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Address the xyz triple for corner `index` in the flat path buffer.
/// @details Path corners are stored as a packed `[x0,y0,z0, x1,y1,z1, ...]`
///   array — returning a stride-adjusted pointer avoids constructing a
///   temporary `double[3]` for every read and lets callers memcpy or index
///   directly. The caller is responsible for bounds-checking `index`
///   against `path_point_count` — this is a raw indexing primitive.
/// @param agent Borrowed agent with an allocated packed path buffer.
/// @param index Zero-based, caller-validated path-corner index.
/// @return Borrowed pointer to the indexed three-double corner.
static const double *navagent_path_point(const rt_navagent3d *agent, int32_t index) {
    return agent->path_points_xyz + (size_t)index * 3u;
}

/// @brief Tear down the current path — free the corner buffer, zero counters, and mark
/// `has_path` false so the next update knows there's nothing to follow. Safe to call
/// when no path is active. Does NOT zero the velocity; the caller handles that if a
/// motion reset is also desired.
/// @param agent Mutable agent whose owned corner buffer and path state are reset.
static void navagent_clear_path(rt_navagent3d *agent) {
    if (!agent)
        return;
    free(agent->path_points_xyz);
    agent->path_points_xyz = NULL;
    agent->path_point_count = 0;
    agent->path_index = 0;
    agent->has_path = 0;
    agent->remaining_distance = 0.0;
}

/// @brief Bring both the current and desired velocity vectors to rest.
/// @details Called when the agent reaches its destination, loses its path,
///   or is forcibly stopped. Clearing *both* velocity vectors matters because
///   the integrator uses `desired_velocity` as the steering target — if only
///   `velocity` were reset, the next step would immediately re-accelerate
///   toward the stale desired velocity and produce a visible jitter.
/// @param agent Mutable agent to stop; null is a no-op.
static void navagent_zero_motion(rt_navagent3d *agent) {
    if (!agent)
        return;
    navagent_vec_set(agent->velocity, 0.0, 0.0, 0.0);
    navagent_vec_set(agent->desired_velocity, 0.0, 0.0, 0.0);
}

/// @brief The agent's avoidance radius if positive and finite, else 0 (avoidance disabled).
/// @param agent Optional borrowed agent.
/// @return Finite effective radius in the supported range, or zero.
static double navagent_effective_avoidance_radius(const rt_navagent3d *agent) {
    return agent ? navagent_nonnegative_capped_or(agent->avoidance_radius, 0.0, NAVAGENT_RADIUS_MAX)
                 : 0.0;
}

/// @brief An agent's interaction "reach": radius plus a bounded RVO time horizon of travel.
///   Two agents can only influence each other within (reachA + reachB), which bounds the grid
///   query.
/// @param agent Optional borrowed agent.
/// @return Finite non-negative interaction reach capped to the navigation distance bound.
static double navagent_reach(const rt_navagent3d *agent) {
    double r = navagent_effective_avoidance_radius(agent);
    double s =
        navagent_nonnegative_capped_or(agent ? agent->desired_speed : 0.0, 0.0, NAVAGENT_SPEED_MAX);
    return navagent_nonnegative_capped_or(
        r + s * NAVAGENT_RVO_MAX_TIME_HORIZON, 0.0, NAVAGENT_DISTANCE_MAX);
}

/// @brief Recompute the global maximum avoidance reach from registered agents.
/// @details Called when an agent is removed or a reach-affecting property
///          shrinks. This keeps grid neighbor-ring queries from using a stale
///          overlarge bound forever after the largest agent changes.
static void navagent_recompute_max_reach(void) {
    double max_reach = 0.0;
    for (rt_navagent3d *agent = g_navagent3d_registry; agent; agent = agent->registry_next) {
        double reach = navagent_reach(agent);
        if (reach > max_reach)
            max_reach = reach;
    }
    g_navagent3d_max_reach = max_reach;
}

/// @brief Quantize a world coordinate to its spatial-grid cell index (clamped to ±1e9; 0 if
/// non-finite).
/// @param v World-space X or Z coordinate.
/// @return Clamped integer cell coordinate.
static int32_t navagent_grid_coord(double v) {
    double c;
    if (!isfinite(v))
        return 0;
    c = floor(v / NAVAGENT_GRID_CELL);
    if (c < -1.0e9)
        c = -1.0e9;
    if (c > 1.0e9)
        c = 1.0e9;
    return (int32_t)c;
}

/// @brief Hash a 2D cell (cx, cz) to a grid bucket using the Teschner spatial-hash primes.
/// @details NAVAGENT_GRID_BUCKETS is a power of two, so the mask replaces a modulo.
/// @param cx Quantized X cell coordinate.
/// @param cz Quantized Z cell coordinate.
/// @return Bucket index in `[0, NAVAGENT_GRID_BUCKETS)`.
static uint32_t navagent_grid_bucket(int32_t cx, int32_t cz) {
    uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cz * 19349663u;
    return h & (NAVAGENT_GRID_BUCKETS - 1u);
}

/// @brief Insert an agent into the spatial grid bucket for its current XZ cell (no-op if already
/// in).
/// @param agent Mutable registered agent; null or already-inserted input is a no-op.
static void navagent_grid_insert(rt_navagent3d *agent) {
    RT_ASSERT_MAIN_THREAD();
    uint32_t b;
    if (!agent || agent->in_grid)
        return;
    agent->grid_cx = navagent_grid_coord(agent->position[0]);
    agent->grid_cz = navagent_grid_coord(agent->position[2]);
    b = navagent_grid_bucket(agent->grid_cx, agent->grid_cz);
    agent->grid_next = g_navagent3d_grid[b];
    g_navagent3d_grid[b] = agent;
    agent->in_grid = 1;
}

/// @brief Unlink an agent from its spatial-grid bucket (no-op if not currently in the grid).
/// @param agent Mutable registered agent; null or absent input is a no-op.
static void navagent_grid_remove(rt_navagent3d *agent) {
    RT_ASSERT_MAIN_THREAD();
    uint32_t b;
    rt_navagent3d **link;
    if (!agent || !agent->in_grid)
        return;
    b = navagent_grid_bucket(agent->grid_cx, agent->grid_cz);
    link = &g_navagent3d_grid[b];
    while (*link) {
        if (*link == agent) {
            *link = agent->grid_next;
            agent->grid_next = NULL;
            break;
        }
        link = &(*link)->grid_next;
    }
    agent->in_grid = 0;
}

/// @brief Keep @p agent's grid cell current and grow the global reach bound. Called after every
///   position sync and on registration; moves the agent between buckets only when its cell changes.
/// @param agent Mutable registered agent; null is a no-op.
static void navagent_grid_refresh(rt_navagent3d *agent) {
    RT_ASSERT_MAIN_THREAD();
    double reach;
    int32_t cx;
    int32_t cz;
    if (!agent)
        return;
    reach = navagent_reach(agent);
    if (reach > g_navagent3d_max_reach)
        g_navagent3d_max_reach = reach;
    if (!agent->in_grid) {
        navagent_grid_insert(agent);
        return;
    }
    cx = navagent_grid_coord(agent->position[0]);
    cz = navagent_grid_coord(agent->position[2]);
    if (cx != agent->grid_cx || cz != agent->grid_cz) {
        navagent_grid_remove(agent);
        navagent_grid_insert(agent);
    }
}

/// @brief Estimate an agent's current preferred XZ velocity for reciprocal avoidance.
/// @details During an update, `desired_velocity` already holds the freshly computed preferred
///   velocity for the current agent. Peers may not have updated yet this frame, so fall back to
///   their current path corner and desired speed before using their previous actual velocity.
/// @param agent Optional borrowed peer agent.
/// @param out_vx Receives the finite preferred X velocity and is cleared before evaluation.
/// @param out_vz Receives the finite preferred Z velocity and is cleared before evaluation.
static void navagent_preferred_velocity_xz(const rt_navagent3d *agent,
                                           double *out_vx,
                                           double *out_vz) {
    double speed;
    if (!out_vx || !out_vz)
        return;
    *out_vx = 0.0;
    *out_vz = 0.0;
    if (!agent)
        return;
    if (isfinite(agent->desired_velocity[0]) && isfinite(agent->desired_velocity[2]) &&
        agent->desired_velocity[0] * agent->desired_velocity[0] +
                agent->desired_velocity[2] * agent->desired_velocity[2] >
            1e-10) {
        *out_vx = agent->desired_velocity[0];
        *out_vz = agent->desired_velocity[2];
        return;
    }
    if (agent->has_path && agent->path_points_xyz && agent->path_point_count > 0) {
        int32_t idx = agent->path_index;
        const double *next;
        double dx;
        double dz;
        double dist;
        if (idx < 0)
            idx = 0;
        if (idx >= agent->path_point_count)
            idx = agent->path_point_count - 1;
        next = navagent_path_point(agent, idx);
        dx = next[0] - agent->position[0];
        dz = next[2] - agent->position[2];
        dist = sqrt(dx * dx + dz * dz);
        speed = (isfinite(agent->desired_speed) && agent->desired_speed > 0.0)
                    ? agent->desired_speed
                    : 0.0;
        if (dist > 1e-9 && speed > 0.0) {
            *out_vx = dx / dist * speed;
            *out_vz = dz / dist * speed;
            return;
        }
    }
    if (agent->has_target) {
        double dx = agent->target[0] - agent->position[0];
        double dz = agent->target[2] - agent->position[2];
        double dist = sqrt(dx * dx + dz * dz);
        speed = (isfinite(agent->desired_speed) && agent->desired_speed > 0.0)
                    ? agent->desired_speed
                    : 0.0;
        if (dist > 1e-9 && speed > 0.0) {
            *out_vx = dx / dist * speed;
            *out_vz = dz / dist * speed;
            return;
        }
    }
    if (isfinite(agent->velocity[0]) && isfinite(agent->velocity[2])) {
        *out_vx = agent->velocity[0];
        *out_vz = agent->velocity[2];
    }
}

/// @brief Add a velocity candidate after speed clamping and near-duplicate removal.
/// @param candidates Caller-owned fixed-capacity candidate array.
/// @param count Current number of initialized candidates.
/// @param vx Candidate X velocity.
/// @param vz Candidate Z velocity.
/// @param max_speed Finite non-negative speed limit.
/// @return Updated candidate count; invalid, duplicate, or over-capacity input leaves it unchanged.
static int navagent_add_velocity_candidate(double candidates[NAVAGENT_RVO_MAX_CANDIDATES][2],
                                           int count,
                                           double vx,
                                           double vz,
                                           double max_speed) {
    double speed_sq;
    if (!isfinite(vx) || !isfinite(vz) || !isfinite(max_speed) || max_speed < 0.0)
        return count;
    speed_sq = vx * vx + vz * vz;
    if (!isfinite(speed_sq) || speed_sq < 0.0)
        return count;
    if (speed_sq > max_speed * max_speed && speed_sq > 1e-12) {
        double scale = max_speed / sqrt(speed_sq);
        vx *= scale;
        vz *= scale;
    }
    for (int i = 0; i < count; ++i) {
        double dx = candidates[i][0] - vx;
        double dz = candidates[i][1] - vz;
        if (dx * dx + dz * dz <= 1e-10)
            return count;
    }
    if (count >= NAVAGENT_RVO_MAX_CANDIDATES)
        return count;
    candidates[count][0] = vx;
    candidates[count][1] = vz;
    return count + 1;
}

/// @brief Build a deterministic RVO candidate set around the preferred velocity.
/// @param pref_x Preferred X velocity.
/// @param pref_z Preferred Z velocity.
/// @param max_speed Non-negative admissible speed.
/// @param candidates Caller-owned fixed-capacity array receiving unique sampled velocities.
/// @return Number of initialized candidate entries.
static int navagent_build_velocity_candidates(double pref_x,
                                              double pref_z,
                                              double max_speed,
                                              double candidates[NAVAGENT_RVO_MAX_CANDIDATES][2]) {
    static const double speed_mults[] = {1.0, 0.75, 0.5};
    static const double angle_offsets[] = {0.0,
                                           0.3490658503988659,
                                           -0.3490658503988659,
                                           0.6981317007977318,
                                           -0.6981317007977318,
                                           1.0471975511965976,
                                           -1.0471975511965976,
                                           1.5707963267948966,
                                           -1.5707963267948966,
                                           2.356194490192345,
                                           -2.356194490192345,
                                           3.141592653589793};
    double pref_len = sqrt(pref_x * pref_x + pref_z * pref_z);
    double base_angle = pref_len > 1e-9 ? atan2(pref_z, pref_x) : 0.0;
    int count = 0;
    count = navagent_add_velocity_candidate(candidates, count, pref_x, pref_z, max_speed);
    count = navagent_add_velocity_candidate(candidates, count, 0.0, 0.0, max_speed);
    for (size_t s = 0; s < sizeof(speed_mults) / sizeof(speed_mults[0]); ++s) {
        double speed = max_speed * speed_mults[s];
        for (size_t a = 0; a < sizeof(angle_offsets) / sizeof(angle_offsets[0]); ++a) {
            double theta = base_angle + angle_offsets[a];
            count = navagent_add_velocity_candidate(
                candidates, count, cos(theta) * speed, sin(theta) * speed, max_speed);
        }
    }
    return count;
}

/// @brief Penalty for choosing candidate velocity (`cand_x`,`cand_z`) against one peer.
/// @details This is a reciprocal velocity-obstacle test: it predicts relative motion over the
///   bounded time horizon and penalizes candidates that enter the combined avoidance disk.
/// @param agent Borrowed agent being solved.
/// @param other Borrowed peer evaluated against @p agent.
/// @param agent_radius Effective avoidance radius for @p agent.
/// @param horizon Positive collision-prediction horizon in seconds.
/// @param cand_x Candidate X velocity for @p agent.
/// @param cand_z Candidate Z velocity for @p agent.
/// @return Non-negative collision-risk penalty; unrelated or non-contributing peers return zero.
static double navagent_rvo_peer_penalty(rt_navagent3d *agent,
                                        rt_navagent3d *other,
                                        double agent_radius,
                                        double horizon,
                                        double cand_x,
                                        double cand_z) {
    double other_radius;
    double combined;
    double rel_px;
    double rel_pz;
    double other_vx = 0.0;
    double other_vz = 0.0;
    double rel_vx;
    double rel_vz;
    double dist_sq;
    double combined_sq;
    double rel_speed_sq;
    double dot;
    double t;
    double closest_x;
    double closest_z;
    double closest_sq;
    if (other == agent || !other->avoidance_enabled || other->navmesh != agent->navmesh)
        return 0.0;
    other_radius = navagent_effective_avoidance_radius(other);
    combined = agent_radius + other_radius;
    if (combined <= 0.0)
        return 0.0;
    rel_px = other->position[0] - agent->position[0];
    rel_pz = other->position[2] - agent->position[2];
    dist_sq = rel_px * rel_px + rel_pz * rel_pz;
    combined_sq = combined * combined;
    navagent_preferred_velocity_xz(other, &other_vx, &other_vz);
    rel_vx = other_vx - cand_x;
    rel_vz = other_vz - cand_z;
    rel_speed_sq = rel_vx * rel_vx + rel_vz * rel_vz;

    if (dist_sq < combined_sq) {
        double dist = dist_sq > 1e-12 ? sqrt(dist_sq) : 0.0;
        double penetration = (combined - dist) / combined;
        double sep_rate = 0.0;
        if (dist > 1e-9)
            sep_rate = (rel_px * rel_vx + rel_pz * rel_vz) / dist;
        else
            sep_rate = -sqrt(cand_x * cand_x + cand_z * cand_z);
        return 2500.0 * penetration * penetration +
               50.0 * fmax(0.0, (combined / horizon) - sep_rate);
    }
    if (rel_speed_sq <= 1e-10)
        return 0.0;
    dot = rel_px * rel_vx + rel_pz * rel_vz;
    t = -dot / rel_speed_sq;
    if (t < 0.0 || t > horizon)
        return 0.0;
    closest_x = rel_px + rel_vx * t;
    closest_z = rel_pz + rel_vz * t;
    closest_sq = closest_x * closest_x + closest_z * closest_z;
    if (closest_sq >= combined_sq)
        return 0.0;
    {
        double closest = closest_sq > 1e-12 ? sqrt(closest_sq) : 0.0;
        double risk = (combined - closest) / combined;
        double time_weight = 1.0 + (horizon - t) / horizon;
        return 1000.0 * risk * risk * time_weight;
    }
}

/// @brief Ensure the agent-owned avoidance-neighbor scratch list has at least @p needed slots.
/// @details The RVO solver reuses the same peer set for every sampled candidate velocity. Keeping
///   the pointer array on the agent removes per-step heap allocation while still allowing dense
///   crowds to grow beyond the default capacity.
/// @param agent Agent whose reusable neighbor storage is being grown.
/// @param needed Minimum required pointer capacity.
/// @return 1 if the buffer is ready, 0 on invalid input, overflow, or allocation failure.
static int navagent_ensure_neighbor_capacity(rt_navagent3d *agent, int32_t needed) {
    rt_navagent3d **grown;
    int32_t new_capacity;
    if (!agent || needed < 0 || agent->avoidance_neighbor_capacity < 0)
        return 0;
    if (needed <= agent->avoidance_neighbor_capacity)
        return 1;
    new_capacity = agent->avoidance_neighbor_capacity > 0 ? agent->avoidance_neighbor_capacity : 32;
    while (new_capacity < needed) {
        int32_t next = new_capacity * 2;
        if (next <= new_capacity)
            return 0;
        new_capacity = next;
    }
    // avoidance_neighbors is intentionally a reusable array of agent pointers.
    // cppcheck-suppress divideSizeof
    if ((size_t)new_capacity > SIZE_MAX / sizeof(*grown))
        return 0;
    grown = (rt_navagent3d **)realloc(agent->avoidance_neighbors,
                                      (size_t)new_capacity * sizeof(*grown));
    if (!grown)
        return 0;
    agent->avoidance_neighbors = grown;
    agent->avoidance_neighbor_capacity = new_capacity;
    return 1;
}

/// @brief Append one avoidance neighbor pointer to an agent-owned reusable list.
///
/// The RVO solver evaluates many candidate velocities against the same peer set.
/// Keeping that set in an agent-owned list avoids rescanning the spatial grid or registry for
/// every candidate and avoids allocating/freeing a temporary list on every tick.
/// @param owner Agent that owns the reusable neighbor pointer array.
/// @param count In/out number of valid entries in the owner list.
/// @param other Neighbor agent to append.
/// @return 1 on success, 0 when inputs are invalid, capacity overflows, or
///         allocation fails; existing list contents remain valid on failure.
static int navagent_neighbor_list_append(rt_navagent3d *owner,
                                         int32_t *count,
                                         rt_navagent3d *other) {
    if (!owner || !count || !other || *count < 0)
        return 0;
    if (!navagent_ensure_neighbor_capacity(owner, *count + 1))
        return 0;
    owner->avoidance_neighbors[*count] = other;
    (*count)++;
    return 1;
}

/// @brief Collect the peer agents that can contribute to one RVO solve.
///
/// Uses the spatial grid when the query radius fits within the bounded ring;
/// otherwise it falls back to the registry walk. Basic filters are applied up
/// front so each candidate only evaluates agents on the same navmesh with
/// avoidance enabled and a positive avoidance radius.
/// @param agent Agent whose avoidance solve is being prepared.
/// @param use_grid Non-zero to try the spatial-grid neighborhood first.
/// @param out_neighbors Receives the agent-owned reusable array of candidate agents. The pointer
///                      remains owned by @p agent and is valid until the next collection/finalize.
/// @param out_count Receives the number of entries in @p out_neighbors.
/// @return 1 when collection succeeds, 0 for invalid inputs or allocation
///         failure.
static int navagent_collect_avoidance_neighbors(rt_navagent3d *agent,
                                                int use_grid,
                                                rt_navagent3d ***out_neighbors,
                                                int32_t *out_count) {
    int32_t count = 0;

    if (out_neighbors)
        *out_neighbors = NULL;
    if (out_count)
        *out_count = 0;
    if (!agent || !out_neighbors || !out_count)
        return 0;

#define NAVAGENT_APPEND_IF_PEER(candidate_)                                                        \
    do {                                                                                           \
        rt_navagent3d *candidate_agent__ = (candidate_);                                           \
        if (candidate_agent__ != agent && candidate_agent__->avoidance_enabled &&                  \
            candidate_agent__->navmesh == agent->navmesh &&                                        \
            navagent_effective_avoidance_radius(candidate_agent__) > 0.0 &&                        \
            !navagent_neighbor_list_append(agent, &count, candidate_agent__)) {                    \
            return 0;                                                                              \
        }                                                                                          \
    } while (0)

    if (use_grid && agent->in_grid) {
        double dmax = navagent_reach(agent) + g_navagent3d_max_reach;
        int ring = (int)ceil(dmax / NAVAGENT_GRID_CELL) + 1;
        /* Use the spatial grid while its cell-scan cost (~(2*ring+1)^2) is no worse than a
         * full registry walk. A single large-reach agent inflates g_navagent3d_max_reach
         * (and thus `ring`) for everyone; comparing costs keeps the grid for the common
         * moderately-inflated case instead of dropping every agent onto the O(N) registry
         * scan at a fixed ring cap. Both paths are exhaustive, so the neighbour set is
         * identical regardless of which is chosen. */
        int64_t grid_cells = (int64_t)(2 * ring + 1) * (int64_t)(2 * ring + 1);
        if (ring >= 1 &&
            (ring <= NAVAGENT_GRID_MAX_RING || grid_cells <= g_navagent3d_registry_count)) {
            int32_t cx = agent->grid_cx;
            int32_t cz = agent->grid_cz;
            for (int32_t dz = -ring; dz <= ring; dz++) {
                for (int32_t dx = -ring; dx <= ring; dx++) {
                    int32_t qx = cx + dx;
                    int32_t qz = cz + dz;
                    uint32_t b = navagent_grid_bucket(qx, qz);
                    for (rt_navagent3d *other = g_navagent3d_grid[b]; other;
                         other = other->grid_next) {
                        if (other->grid_cx == qx && other->grid_cz == qz)
                            NAVAGENT_APPEND_IF_PEER(other);
                    }
                }
            }
            *out_neighbors = agent->avoidance_neighbors;
            *out_count = count;
            return 1;
        }
        /* reach too large for a tight neighborhood: fall through to the full registry scan */
    }

    for (rt_navagent3d *other = g_navagent3d_registry; other; other = other->registry_next)
        NAVAGENT_APPEND_IF_PEER(other);
    *out_neighbors = agent->avoidance_neighbors;
    *out_count = count;
#undef NAVAGENT_APPEND_IF_PEER
    return 1;
}

/// @brief Score one candidate velocity against a pre-collected RVO neighbor list.
/// @param agent Borrowed agent being solved.
/// @param neighbors Borrowed array of candidate peer pointers.
/// @param neighbor_count Number of entries in @p neighbors.
/// @param agent_radius Effective avoidance radius for @p agent.
/// @param horizon Positive collision-prediction horizon in seconds.
/// @param cand_x Candidate X velocity.
/// @param cand_z Candidate Z velocity.
/// @return Sum of all contributing peer penalties, or zero for an empty list.
static double navagent_rvo_neighbor_list_penalty(rt_navagent3d *agent,
                                                 rt_navagent3d *const *neighbors,
                                                 int32_t neighbor_count,
                                                 double agent_radius,
                                                 double horizon,
                                                 double cand_x,
                                                 double cand_z) {
    double penalty = 0.0;
    if (!neighbors || neighbor_count <= 0)
        return 0.0;
    for (int32_t i = 0; i < neighbor_count; i++) {
        penalty +=
            navagent_rvo_peer_penalty(agent, neighbors[i], agent_radius, horizon, cand_x, cand_z);
    }
    return penalty;
}

/// @brief Evaluate one candidate velocity against the grid or full registry.
/// @param agent Borrowed registered agent being solved.
/// @param agent_radius Effective avoidance radius for @p agent.
/// @param horizon Positive collision-prediction horizon in seconds.
/// @param use_grid Non-zero to prefer a bounded spatial-grid scan when cheaper.
/// @param cand_x Candidate X velocity.
/// @param cand_z Candidate Z velocity.
/// @return Accumulated peer-collision penalty.
static double navagent_rvo_candidate_penalty(rt_navagent3d *agent,
                                             double agent_radius,
                                             double horizon,
                                             int use_grid,
                                             double cand_x,
                                             double cand_z) {
    rt_navagent3d *other;
    double penalty = 0.0;
    if (use_grid && agent->in_grid) {
        double dmax = navagent_reach(agent) + g_navagent3d_max_reach;
        int ring = (int)ceil(dmax / NAVAGENT_GRID_CELL) + 1;
        /* See navagent_collect_avoidance_neighbors: prefer the grid while its scan cost
         * (~(2*ring+1)^2 cells) is no worse than walking all g_navagent3d_registry_count
         * agents, rather than falling back at a fixed ring cap. Both paths are exhaustive. */
        int64_t grid_cells = (int64_t)(2 * ring + 1) * (int64_t)(2 * ring + 1);
        if (ring >= 1 &&
            (ring <= NAVAGENT_GRID_MAX_RING || grid_cells <= g_navagent3d_registry_count)) {
            int32_t cx = agent->grid_cx;
            int32_t cz = agent->grid_cz;
            int32_t dz;
            for (dz = -ring; dz <= ring; dz++) {
                int32_t dx;
                for (dx = -ring; dx <= ring; dx++) {
                    int32_t qx = cx + dx;
                    int32_t qz = cz + dz;
                    uint32_t b = navagent_grid_bucket(qx, qz);
                    for (other = g_navagent3d_grid[b]; other; other = other->grid_next) {
                        if (other->grid_cx == qx && other->grid_cz == qz) {
                            penalty += navagent_rvo_peer_penalty(
                                agent, other, agent_radius, horizon, cand_x, cand_z);
                        }
                    }
                }
            }
            return penalty;
        }
        /* reach too large for a tight neighborhood: fall through to the full scan */
    }
    for (other = g_navagent3d_registry; other; other = other->registry_next)
        penalty += navagent_rvo_peer_penalty(agent, other, agent_radius, horizon, cand_x, cand_z);
    return penalty;
}

/// @brief Compute a reciprocal velocity-obstacle steering adjustment for @p agent.
/// @details The solver samples a deterministic set of admissible velocities around the preferred
///   path-following velocity, scores each against predicted peer collisions over a bounded time
///   horizon, and returns the delta from preferred to the best candidate. With @p use_grid it scans
///   only the spatial-grid cell neighborhood sized to cover the RVO horizon.
/// @param agent Mutable agent whose preferred velocity is evaluated.
/// @param dt Current bounded simulation step in seconds.
/// @param use_grid Non-zero to prefer spatial-grid peer discovery.
/// @param out_ax Receives the chosen X-velocity adjustment on success.
/// @param out_az Receives the chosen Z-velocity adjustment on success.
/// @return 1 if avoidance applies and outputs were written, otherwise 0.
static int navagent_compute_avoidance_adjust(
    rt_navagent3d *agent, double dt, int use_grid, double *out_ax, double *out_az) {
    double agent_radius;
    double max_speed;
    double horizon;
    double pref_x;
    double pref_z;
    double pref_len;
    double right_x = 0.0;
    double right_z = 0.0;
    double candidates[NAVAGENT_RVO_MAX_CANDIDATES][2];
    int candidate_count;
    double best_x;
    double best_z;
    double best_score = 1.0e300;
    rt_navagent3d **neighbors = NULL;
    int32_t neighbor_count = 0;
    int use_neighbor_list = 0;
    if (!agent || !agent->avoidance_enabled)
        return 0;
    agent_radius = navagent_effective_avoidance_radius(agent);
    max_speed =
        (isfinite(agent->desired_speed) && agent->desired_speed > 0.0) ? agent->desired_speed : 0.0;
    if (agent_radius <= 0.0 || max_speed <= 0.0)
        return 0;
    horizon = (isfinite(dt) && dt > 0.0) ? dt * 12.0 : NAVAGENT_RVO_MIN_TIME_HORIZON;
    if (horizon < NAVAGENT_RVO_MIN_TIME_HORIZON)
        horizon = NAVAGENT_RVO_MIN_TIME_HORIZON;
    if (horizon > NAVAGENT_RVO_MAX_TIME_HORIZON)
        horizon = NAVAGENT_RVO_MAX_TIME_HORIZON;
    pref_x = agent->desired_velocity[0];
    pref_z = agent->desired_velocity[2];
    if (!isfinite(pref_x) || !isfinite(pref_z))
        return 0;
    pref_len = sqrt(pref_x * pref_x + pref_z * pref_z);
    if (pref_len <= 1e-9)
        return 0;
    right_x = pref_z / pref_len;
    right_z = -pref_x / pref_len;
    candidate_count = navagent_build_velocity_candidates(pref_x, pref_z, max_speed, candidates);
    use_neighbor_list =
        navagent_collect_avoidance_neighbors(agent, use_grid, &neighbors, &neighbor_count);
    best_x = pref_x;
    best_z = pref_z;
    for (int i = 0; i < candidate_count; ++i) {
        double cand_x = candidates[i][0];
        double cand_z = candidates[i][1];
        double dx = cand_x - pref_x;
        double dz = cand_z - pref_z;
        double cand_speed = sqrt(cand_x * cand_x + cand_z * cand_z);
        double score = dx * dx + dz * dz;
        double forward = cand_x * pref_x + cand_z * pref_z;
        double side = cand_x * right_x + cand_z * right_z;
        double speed_loss = max_speed - cand_speed;
        score += use_neighbor_list
                     ? navagent_rvo_neighbor_list_penalty(
                           agent, neighbors, neighbor_count, agent_radius, horizon, cand_x, cand_z)
                     : navagent_rvo_candidate_penalty(
                           agent, agent_radius, horizon, use_grid, cand_x, cand_z);
        if (speed_loss > 0.0)
            score += speed_loss * speed_loss * 1.5;
        if (forward < 0.0)
            score += max_speed * max_speed * 2.0;
        /* Stable reciprocal passing side: prefer the candidate to the agent's right only as a
         * tie-breaker. Opposite headings therefore choose opposite world-space sides. */
        score -= side * max_speed * 1e-4;
        if (score < best_score) {
            best_score = score;
            best_x = cand_x;
            best_z = cand_z;
        }
    }
    *out_ax = best_x - pref_x;
    *out_az = best_z - pref_z;
    return 1;
}

/// @brief Steer the agent's desired velocity to avoid nearby peers.
/// @details Gathers peers via the spatial grid and applies a reciprocal velocity-obstacle candidate
///   solution. The selected candidate stays within DesiredSpeed, favors the preferred path
///   velocity, and predicts peer collisions across a bounded time horizon instead of only pushing
///   away after overlap.
/// @param agent Mutable agent whose desired velocity is adjusted.
/// @param dt Current bounded simulation step in seconds.
static void navagent_apply_local_avoidance(rt_navagent3d *agent, double dt) {
    double max_speed;
    double adjust_x = 0.0;
    double adjust_z = 0.0;
    if (!navagent_compute_avoidance_adjust(agent, dt, 1, &adjust_x, &adjust_z))
        return;
    max_speed =
        (isfinite(agent->desired_speed) && agent->desired_speed > 0.0) ? agent->desired_speed : 0.0;
    if (fabs(adjust_x) + fabs(adjust_z) <= 1e-10)
        return;
    agent->desired_velocity[0] += adjust_x;
    agent->desired_velocity[2] += adjust_z;
    {
        double speed_sq = navagent_len_sq(agent->desired_velocity);
        double max_speed_sq = max_speed * max_speed;
        if (speed_sq > max_speed_sq && speed_sq > 1e-10) {
            double scale = max_speed / sqrt(speed_sq);
            agent->desired_velocity[0] *= scale;
            agent->desired_velocity[1] *= scale;
            agent->desired_velocity[2] *= scale;
        }
    }
}

/// @brief Immutable start-of-tick state consumed by one batch avoidance solve.
/// @details Every live agent is represented so selected agents see both selected and unselected
///   peers at one consistent instant. The snapshot carries the explicit position and velocity as
///   well as the preferred velocity derived after path preparation.
typedef struct {
    rt_navagent3d *agent;
    void *navmesh;
    uint64_t stable_order;
    double position[3];
    double velocity[3];
    double desired_velocity[3];
    double preferred_x;
    double preferred_z;
    double desired_speed;
    double avoidance_radius;
    double reach;
    int32_t grid_cx;
    int32_t grid_cz;
    int32_t grid_next;
    int8_t avoidance_enabled;
} navagent_batch_snapshot_t;

/// @brief One selected agent's staged preparation, solved velocity, and apply inputs.
typedef struct {
    rt_navagent3d *agent;
    uint64_t stable_order;
    double previous_position[3];
    double solved_velocity[3];
    double stopping_distance;
    int32_t snapshot_index;
    int8_t should_apply;
} navagent_batch_item_t;

/// @brief Read-only lookup context for a complete batch solve.
typedef struct {
    navagent_batch_snapshot_t *snapshots;
    int32_t snapshot_count;
    int32_t bucket_heads[NAVAGENT_GRID_BUCKETS];
    double max_reach;
} navagent_batch_context_t;

/* Main-thread reusable staging buffers remove per-frame allocation churn for crowd updates. They
 * are process-lifetime scratch, do not retain the raw object pointers they temporarily contain,
 * and are overwritten by every batch. */
static navagent_batch_item_t *g_navagent_batch_items = NULL;
static int32_t g_navagent_batch_item_capacity = 0;
static navagent_batch_snapshot_t *g_navagent_batch_snapshots = NULL;
static int32_t g_navagent_batch_snapshot_capacity = 0;
static int32_t *g_navagent_batch_neighbor_indices = NULL;
static int32_t g_navagent_batch_neighbor_capacity = 0;
static int g_navagent_batch_active = 0;

/// @brief Grow the reusable selected-agent staging array.
/// @param needed Minimum number of batch-item records required.
/// @return 1 when capacity is available, or 0 on invalid size/overflow/allocation failure.
static int navagent_batch_reserve_items(int32_t needed) {
    navagent_batch_item_t *grown;
    int32_t capacity;
    if (needed < 0)
        return 0;
    if (needed <= g_navagent_batch_item_capacity)
        return 1;
    capacity = g_navagent_batch_item_capacity > 0 ? g_navagent_batch_item_capacity : 16;
    while (capacity < needed) {
        if (capacity > INT32_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof(*grown))
        return 0;
    grown =
        (navagent_batch_item_t *)realloc(g_navagent_batch_items, (size_t)capacity * sizeof(*grown));
    if (!grown)
        return 0;
    g_navagent_batch_items = grown;
    g_navagent_batch_item_capacity = capacity;
    return 1;
}

/// @brief Grow the reusable whole-registry snapshot array.
/// @param needed Minimum number of immutable snapshot records required.
/// @return 1 when capacity is available, or 0 on invalid size/overflow/allocation failure.
static int navagent_batch_reserve_snapshots(int32_t needed) {
    navagent_batch_snapshot_t *grown;
    int32_t capacity;
    if (needed < 0)
        return 0;
    if (needed <= g_navagent_batch_snapshot_capacity)
        return 1;
    capacity = g_navagent_batch_snapshot_capacity > 0 ? g_navagent_batch_snapshot_capacity : 32;
    while (capacity < needed) {
        if (capacity > INT32_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof(*grown))
        return 0;
    grown = (navagent_batch_snapshot_t *)realloc(g_navagent_batch_snapshots,
                                                 (size_t)capacity * sizeof(*grown));
    if (!grown)
        return 0;
    g_navagent_batch_snapshots = grown;
    g_navagent_batch_snapshot_capacity = capacity;
    return 1;
}

/// @brief Grow the reusable snapshot-neighbor index list.
/// @param needed Minimum number of peer indices required by an exhaustive solve.
/// @return 1 when capacity is available, or 0 on invalid size/overflow/allocation failure.
static int navagent_batch_reserve_neighbors(int32_t needed) {
    int32_t *grown;
    int32_t capacity;
    if (needed < 0)
        return 0;
    if (needed <= g_navagent_batch_neighbor_capacity)
        return 1;
    capacity = g_navagent_batch_neighbor_capacity > 0 ? g_navagent_batch_neighbor_capacity : 32;
    while (capacity < needed) {
        if (capacity > INT32_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof(*grown))
        return 0;
    grown =
        (int32_t *)realloc(g_navagent_batch_neighbor_indices, (size_t)capacity * sizeof(*grown));
    if (!grown)
        return 0;
    g_navagent_batch_neighbor_indices = grown;
    g_navagent_batch_neighbor_capacity = capacity;
    return 1;
}

/// @brief Compare selected batch items by stable creation order, then numeric handle value.
/// @param lhs Pointer to a `navagent_batch_item_t`.
/// @param rhs Pointer to a `navagent_batch_item_t`.
/// @return Negative/zero/positive according to deterministic handle order.
static int navagent_batch_item_compare(const void *lhs, const void *rhs) {
    const navagent_batch_item_t *a = (const navagent_batch_item_t *)lhs;
    const navagent_batch_item_t *b = (const navagent_batch_item_t *)rhs;
    if (a->stable_order < b->stable_order)
        return -1;
    if (a->stable_order > b->stable_order)
        return 1;
    uintptr_t ap = (uintptr_t)a->agent;
    uintptr_t bp = (uintptr_t)b->agent;
    return ap < bp ? -1 : ap > bp ? 1 : 0;
}

/// @brief Compare immutable snapshots by stable creation order, then numeric handle value.
/// @param lhs Pointer to a `navagent_batch_snapshot_t`.
/// @param rhs Pointer to a `navagent_batch_snapshot_t`.
/// @return Negative/zero/positive according to deterministic handle order.
static int navagent_batch_snapshot_compare(const void *lhs, const void *rhs) {
    const navagent_batch_snapshot_t *a = (const navagent_batch_snapshot_t *)lhs;
    const navagent_batch_snapshot_t *b = (const navagent_batch_snapshot_t *)rhs;
    if (a->stable_order < b->stable_order)
        return -1;
    if (a->stable_order > b->stable_order)
        return 1;
    uintptr_t ap = (uintptr_t)a->agent;
    uintptr_t bp = (uintptr_t)b->agent;
    return ap < bp ? -1 : ap > bp ? 1 : 0;
}

/// @brief Locate one selected live agent in the stable-order snapshot array.
/// @param snapshots Sorted immutable snapshot array.
/// @param count Number of valid snapshots.
/// @param agent Agent pointer to locate.
/// @return Snapshot index, or -1 when absent.
static int32_t navagent_batch_find_snapshot(const navagent_batch_snapshot_t *snapshots,
                                            int32_t count,
                                            const rt_navagent3d *agent) {
    int32_t low = 0;
    int32_t high = count;
    if (!snapshots || count <= 0 || !agent)
        return -1;
    while (low < high) {
        int32_t mid = low + (high - low) / 2;
        if (snapshots[mid].stable_order < agent->stable_order)
            low = mid + 1;
        else
            high = mid;
    }
    for (int32_t i = low; i < count && snapshots[i].stable_order == agent->stable_order; i++)
        if (snapshots[i].agent == agent)
            return i;
    return -1;
}

/// @brief Populate and spatially index an immutable snapshot of every registered agent.
/// @details Snapshots are sorted by stable handle order before hash insertion, so both full scans
///   and hash-bucket walks have deterministic order independent of the caller's selected array.
/// @param context Output context whose fixed bucket table and reusable snapshot pointer are filled.
/// @return 1 on success, or 0 if registry accounting exceeds reserved bounds.
static int navagent_batch_build_snapshot(navagent_batch_context_t *context) {
    int32_t count = 0;
    if (!context || g_navagent3d_registry_count < 0 || g_navagent3d_registry_count > INT32_MAX)
        return 0;
    for (rt_navagent3d *agent = g_navagent3d_registry; agent; agent = agent->registry_next) {
        if (count >= g_navagent_batch_snapshot_capacity)
            return 0;
        navagent_batch_snapshot_t *snapshot = &g_navagent_batch_snapshots[count++];
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->agent = agent;
        snapshot->navmesh = agent->navmesh;
        snapshot->stable_order = agent->stable_order;
        navagent_vec_copy(snapshot->position, agent->position);
        navagent_vec_copy(snapshot->velocity, agent->velocity);
        navagent_vec_copy(snapshot->desired_velocity, agent->desired_velocity);
        navagent_preferred_velocity_xz(agent, &snapshot->preferred_x, &snapshot->preferred_z);
        snapshot->desired_speed =
            navagent_nonnegative_capped_or(agent->desired_speed, 0.0, NAVAGENT_SPEED_MAX);
        snapshot->avoidance_radius = navagent_effective_avoidance_radius(agent);
        snapshot->reach = navagent_reach(agent);
        snapshot->grid_cx = navagent_grid_coord(snapshot->position[0]);
        snapshot->grid_cz = navagent_grid_coord(snapshot->position[2]);
        snapshot->grid_next = -1;
        snapshot->avoidance_enabled = agent->avoidance_enabled ? 1 : 0;
    }
    qsort(g_navagent_batch_snapshots,
          (size_t)count,
          sizeof(*g_navagent_batch_snapshots),
          navagent_batch_snapshot_compare);
    context->snapshots = g_navagent_batch_snapshots;
    context->snapshot_count = count;
    context->max_reach = 0.0;
    for (uint32_t bucket = 0; bucket < NAVAGENT_GRID_BUCKETS; bucket++)
        context->bucket_heads[bucket] = -1;
    for (int32_t i = 0; i < count; i++) {
        navagent_batch_snapshot_t *snapshot = &context->snapshots[i];
        uint32_t bucket = navagent_grid_bucket(snapshot->grid_cx, snapshot->grid_cz);
        snapshot->grid_next = context->bucket_heads[bucket];
        context->bucket_heads[bucket] = i;
        if (snapshot->reach > context->max_reach)
            context->max_reach = snapshot->reach;
    }
    return 1;
}

/// @brief Append a qualifying immutable peer index to the reusable neighbor list.
/// @param context Snapshot context containing @p candidate_index.
/// @param self_index Snapshot index of the agent being solved.
/// @param candidate_index Potential peer snapshot index.
/// @param count In/out number of appended peers.
/// @return 1 on success, or 0 for corrupt indices/capacity.
static int navagent_batch_append_peer(const navagent_batch_context_t *context,
                                      int32_t self_index,
                                      int32_t candidate_index,
                                      int32_t *count) {
    if (!context || !count || self_index < 0 || candidate_index < 0 ||
        self_index >= context->snapshot_count || candidate_index >= context->snapshot_count ||
        *count < 0 || *count >= g_navagent_batch_neighbor_capacity)
        return 0;
    const navagent_batch_snapshot_t *self = &context->snapshots[self_index];
    const navagent_batch_snapshot_t *candidate = &context->snapshots[candidate_index];
    if (candidate_index == self_index || !candidate->avoidance_enabled ||
        candidate->navmesh != self->navmesh || candidate->avoidance_radius <= 0.0)
        return 1;
    g_navagent_batch_neighbor_indices[(*count)++] = candidate_index;
    return 1;
}

/// @brief Collect a deterministic spatially culled peer list from immutable snapshots.
/// @param context Complete start-of-tick snapshot and spatial hash.
/// @param self_index Snapshot index of the selected agent.
/// @param out_count Receives the number of indices in `g_navagent_batch_neighbor_indices`.
/// @return 1 on success, or 0 for invalid state/capacity.
static int navagent_batch_collect_peers(const navagent_batch_context_t *context,
                                        int32_t self_index,
                                        int32_t *out_count) {
    int32_t count = 0;
    if (out_count)
        *out_count = 0;
    if (!context || !out_count || self_index < 0 || self_index >= context->snapshot_count)
        return 0;
    const navagent_batch_snapshot_t *self = &context->snapshots[self_index];
    double max_distance = self->reach + context->max_reach;
    int ring = (int)ceil(max_distance / NAVAGENT_GRID_CELL) + 1;
    int64_t grid_cells = (int64_t)(2 * ring + 1) * (int64_t)(2 * ring + 1);
    if (ring >= 1 && (ring <= NAVAGENT_GRID_MAX_RING || grid_cells <= context->snapshot_count)) {
        for (int32_t dz = -ring; dz <= ring; dz++) {
            for (int32_t dx = -ring; dx <= ring; dx++) {
                int32_t cell_x = self->grid_cx + dx;
                int32_t cell_z = self->grid_cz + dz;
                uint32_t bucket = navagent_grid_bucket(cell_x, cell_z);
                for (int32_t candidate_index = context->bucket_heads[bucket]; candidate_index >= 0;
                     candidate_index = context->snapshots[candidate_index].grid_next) {
                    const navagent_batch_snapshot_t *candidate =
                        &context->snapshots[candidate_index];
                    if (candidate->grid_cx == cell_x && candidate->grid_cz == cell_z &&
                        !navagent_batch_append_peer(context, self_index, candidate_index, &count))
                        return 0;
                }
            }
        }
    } else {
        for (int32_t candidate_index = 0; candidate_index < context->snapshot_count;
             candidate_index++) {
            if (!navagent_batch_append_peer(context, self_index, candidate_index, &count))
                return 0;
        }
    }
    *out_count = count;
    return 1;
}

/// @brief Score one candidate velocity against one immutable peer snapshot.
/// @details The formula mirrors individual RVO solving but reads no live agent field, ensuring an
///   earlier solve/apply cannot influence a later one in the same batch.
/// @param self Start-of-tick state for the agent being solved.
/// @param other Start-of-tick peer state.
/// @param horizon Bounded prediction horizon.
/// @param candidate_x Candidate X velocity.
/// @param candidate_z Candidate Z velocity.
/// @return Non-negative collision-risk penalty.
static double navagent_batch_peer_penalty(const navagent_batch_snapshot_t *self,
                                          const navagent_batch_snapshot_t *other,
                                          double horizon,
                                          double candidate_x,
                                          double candidate_z) {
    double combined;
    double relative_x;
    double relative_z;
    double relative_velocity_x;
    double relative_velocity_z;
    double distance_sq;
    double combined_sq;
    double relative_speed_sq;
    double dot;
    double time;
    double closest_x;
    double closest_z;
    double closest_sq;
    if (!self || !other || self == other || !other->avoidance_enabled ||
        other->navmesh != self->navmesh)
        return 0.0;
    combined = self->avoidance_radius + other->avoidance_radius;
    if (combined <= 0.0)
        return 0.0;
    relative_x = other->position[0] - self->position[0];
    relative_z = other->position[2] - self->position[2];
    distance_sq = relative_x * relative_x + relative_z * relative_z;
    combined_sq = combined * combined;
    relative_velocity_x = other->preferred_x - candidate_x;
    relative_velocity_z = other->preferred_z - candidate_z;
    relative_speed_sq =
        relative_velocity_x * relative_velocity_x + relative_velocity_z * relative_velocity_z;
    if (distance_sq < combined_sq) {
        double distance = distance_sq > 1e-12 ? sqrt(distance_sq) : 0.0;
        double penetration = (combined - distance) / combined;
        double separation_rate;
        if (distance > 1e-9) {
            separation_rate =
                (relative_x * relative_velocity_x + relative_z * relative_velocity_z) / distance;
        } else {
            separation_rate = -sqrt(candidate_x * candidate_x + candidate_z * candidate_z);
        }
        return 2500.0 * penetration * penetration +
               50.0 * fmax(0.0, (combined / horizon) - separation_rate);
    }
    if (relative_speed_sq <= 1e-10)
        return 0.0;
    dot = relative_x * relative_velocity_x + relative_z * relative_velocity_z;
    time = -dot / relative_speed_sq;
    if (time < 0.0 || time > horizon)
        return 0.0;
    closest_x = relative_x + relative_velocity_x * time;
    closest_z = relative_z + relative_velocity_z * time;
    closest_sq = closest_x * closest_x + closest_z * closest_z;
    if (closest_sq >= combined_sq)
        return 0.0;
    double closest = closest_sq > 1e-12 ? sqrt(closest_sq) : 0.0;
    double risk = (combined - closest) / combined;
    double time_weight = 1.0 + (horizon - time) / horizon;
    return 1000.0 * risk * risk * time_weight;
}

/// @brief Solve one selected agent's final desired velocity from immutable batch state.
/// @param context Complete start-of-tick snapshot context.
/// @param snapshot_index Snapshot index of the selected agent.
/// @param dt Sanitized positive tick duration.
/// @param out_velocity Receives the solved XYZ desired velocity.
/// @return 1 after writing a solution, or 0 for invalid state/peer collection failure.
static int navagent_batch_solve_velocity(const navagent_batch_context_t *context,
                                         int32_t snapshot_index,
                                         double dt,
                                         double out_velocity[3]) {
    double candidates[NAVAGENT_RVO_MAX_CANDIDATES][2];
    double best_x;
    double best_z;
    double best_score = 1.0e300;
    double horizon;
    double preferred_length;
    double right_x;
    double right_z;
    int32_t neighbor_count = 0;
    if (!context || !out_velocity || snapshot_index < 0 ||
        snapshot_index >= context->snapshot_count)
        return 0;
    const navagent_batch_snapshot_t *self = &context->snapshots[snapshot_index];
    navagent_vec_copy(out_velocity, self->desired_velocity);
    if (!self->avoidance_enabled || self->avoidance_radius <= 0.0 || self->desired_speed <= 0.0)
        return 1;
    double preferred_x = self->desired_velocity[0];
    double preferred_z = self->desired_velocity[2];
    preferred_length = sqrt(preferred_x * preferred_x + preferred_z * preferred_z);
    if (!isfinite(preferred_length) || preferred_length <= 1e-9)
        return 1;
    if (!navagent_batch_collect_peers(context, snapshot_index, &neighbor_count))
        return 0;
    horizon = dt * 12.0;
    if (horizon < NAVAGENT_RVO_MIN_TIME_HORIZON)
        horizon = NAVAGENT_RVO_MIN_TIME_HORIZON;
    if (horizon > NAVAGENT_RVO_MAX_TIME_HORIZON)
        horizon = NAVAGENT_RVO_MAX_TIME_HORIZON;
    right_x = preferred_z / preferred_length;
    right_z = -preferred_x / preferred_length;
    int candidate_count = navagent_build_velocity_candidates(
        preferred_x, preferred_z, self->desired_speed, candidates);
    best_x = preferred_x;
    best_z = preferred_z;
    for (int candidate = 0; candidate < candidate_count; candidate++) {
        double candidate_x = candidates[candidate][0];
        double candidate_z = candidates[candidate][1];
        double dx = candidate_x - preferred_x;
        double dz = candidate_z - preferred_z;
        double candidate_speed = sqrt(candidate_x * candidate_x + candidate_z * candidate_z);
        double score = dx * dx + dz * dz;
        double forward = candidate_x * preferred_x + candidate_z * preferred_z;
        double side = candidate_x * right_x + candidate_z * right_z;
        double speed_loss = self->desired_speed - candidate_speed;
        for (int32_t peer = 0; peer < neighbor_count; peer++) {
            score += navagent_batch_peer_penalty(
                self,
                &context->snapshots[g_navagent_batch_neighbor_indices[peer]],
                horizon,
                candidate_x,
                candidate_z);
        }
        if (speed_loss > 0.0)
            score += speed_loss * speed_loss * 1.5;
        if (forward < 0.0)
            score += self->desired_speed * self->desired_speed * 2.0;
        score -= side * self->desired_speed * 1e-4;
        if (score < best_score) {
            best_score = score;
            best_x = candidate_x;
            best_z = candidate_z;
        }
    }
    out_velocity[0] = best_x;
    out_velocity[2] = best_z;
    double speed_sq = navagent_len_sq(out_velocity);
    double max_speed_sq = self->desired_speed * self->desired_speed;
    if (speed_sq > max_speed_sq && speed_sq > 1e-10) {
        double scale = self->desired_speed / sqrt(speed_sq);
        out_velocity[0] *= scale;
        out_velocity[1] *= scale;
        out_velocity[2] *= scale;
    }
    return 1;
}

/// @brief Test-only: verify the spatial-grid avoidance query produces the same steering adjustment
///   as a full registry scan for every registered agent (they must agree up to floating-point
///   summation order). @return 1 if all agents agree (or none registered), 0 on any mismatch. Not
///   part of the scripting surface.
int8_t rt_navagent3d_check_avoidance_grid_parity(void) {
    rt_navagent3d *a;
    for (a = g_navagent3d_registry; a; a = a->registry_next) {
        double gx = 0.0;
        double gz = 0.0;
        double fx = 0.0;
        double fz = 0.0;
        int g = navagent_compute_avoidance_adjust(a, 1.0 / 60.0, 1, &gx, &gz);
        int f = navagent_compute_avoidance_adjust(a, 1.0 / 60.0, 0, &fx, &fz);
        if (g != f)
            return 0;
        if (g && (fabs(gx - fx) > 1e-6 || fabs(gz - fz) > 1e-6))
            return 0;
    }
    return 1;
}

/// @brief Derive "close enough to this corner" distance from the agent's radius. Half
/// the radius works well in practice, with a 0.15 unit floor so tiny agents (e.g. radius
/// 0.2) don't wind up chasing corners forever because their tolerance converged to zero.
/// @param agent Borrowed valid agent.
/// @return Positive path-corner tolerance in world units.
static double navagent_corner_tolerance(const rt_navagent3d *agent) {
    double tol = agent->radius > 0.0 ? agent->radius * 0.5 : 0.2;
    if (tol < 0.15)
        tol = 0.15;
    return tol;
}

/// @brief Snap a world-space query point onto the bound navmesh (closest valid point on
/// any polygon). Falls through with an identity copy — or zero — when no navmesh is bound
/// or when the mesh's closest-point query fails. Used to keep the agent's position, start
/// of the path, and goal all on the navigable surface regardless of where the caller's
/// coordinates originated.
/// @param agent Mutable agent providing the optional NavMesh3D binding.
/// @param src Optional borrowed world-space point; null is treated as the origin.
/// @param dst Caller-owned point receiving the sampled or sanitized input.
static void navagent_sample_point(rt_navagent3d *agent, const double src[3], double dst[3]) {
    if (!agent || !dst) {
        return;
    }
    double clean[3];
    navagent_vec_copy(clean, src);
    if (!agent->navmesh || !rt_g3d_has_class(agent->navmesh, RT_G3D_NAVMESH3D_CLASS_ID)) {
        agent->navmesh = NULL;
        navagent_vec_copy(dst, clean);
        return;
    }
    void *query = rt_vec3_new(clean[0], clean[1], clean[2]);
    void *sample = query ? rt_navmesh3d_sample_position(agent->navmesh, query) : NULL;
    navagent_release_local(query);
    if (!sample) {
        navagent_vec_copy(dst, clean);
        return;
    }
    navagent_vec_set(dst, rt_vec3_x(sample), rt_vec3_y(sample), rt_vec3_z(sample));
    navagent_release_local(sample);
}

/// @brief Resolve a SceneNode3D's world-space position without allocating matrix/vector wrappers.
/// @param node Borrowed SceneNode3D handle; null yields the origin.
/// @param out_pos Caller-owned three-component world-position output.
static void navagent_get_node_world_position(void *node, double out_pos[3]) {
    if (!node) {
        navagent_vec_set(out_pos, 0.0, 0.0, 0.0);
        return;
    }
    if (!rt_scene_node3d_get_world_position_components(
            node, &out_pos[0], &out_pos[1], &out_pos[2])) {
        void *local = rt_scene_node3d_get_position(node);
        navagent_vec_set(out_pos,
                         local ? rt_vec3_x(local) : 0.0,
                         local ? rt_vec3_y(local) : 0.0,
                         local ? rt_vec3_z(local) : 0.0);
        navagent_release_local(local);
    } else {
        navagent_vec_copy(out_pos, out_pos);
    }
}

/// @brief Write a world-space position into a SceneNode3D, converting through the
/// inverse of the parent's world matrix so the node-local coordinate comes out right
/// after the parent transform is applied. When the node is a root (no parent), the
/// world position is written directly as the local position. Also falls back to direct
/// write when the parent's world matrix can't be inverted (degenerate scale, etc.).
/// @param node Borrowed SceneNode3D handle to reposition.
/// @param world_pos Borrowed desired world-space position.
static void navagent_set_node_world_position(void *node, const double world_pos[3]) {
    void *parent;
    if (!node || !world_pos)
        return;
    double clean_world[3];
    navagent_vec_copy(clean_world, world_pos);
    parent = rt_scene_node3d_get_parent(node);
    if (!parent) {
        rt_scene_node3d_set_position(node, clean_world[0], clean_world[1], clean_world[2]);
        return;
    }

    {
        void *parent_world = rt_scene_node3d_get_world_matrix(parent);
        void *parent_inv = parent_world ? rt_mat4_try_inverse(parent_world) : NULL;
        void *world_vec = rt_vec3_new(clean_world[0], clean_world[1], clean_world[2]);
        void *local =
            (parent_inv && world_vec) ? rt_mat4_transform_point(parent_inv, world_vec) : NULL;
        if (local) {
            rt_scene_node3d_set_position(
                node, rt_vec3_x(local), rt_vec3_y(local), rt_vec3_z(local));
        } else {
            rt_scene_node3d_set_position(node, clean_world[0], clean_world[1], clean_world[2]);
        }
        navagent_release_local(local);
        navagent_release_local(world_vec);
        navagent_release_local(parent_inv);
        navagent_release_local(parent_world);
    }
}

/// @brief Pull the agent's authoritative world position from whichever binding is
/// primary. A bound CharacterController3D wins (the controller owns the physics position);
/// otherwise a bound SceneNode3D provides the world transform. With neither binding the
/// agent's cached position is already authoritative and this call is a no-op. Called at
/// the top of every `_update` so external movement of the character or node flows into
/// pathing decisions.
/// @param agent Mutable agent whose cached position and grid cell are refreshed.
static void navagent_sync_position_from_bindings(rt_navagent3d *agent) {
    if (!agent)
        return;
    if (agent->bound_character &&
        !rt_g3d_has_class(agent->bound_character, RT_G3D_CHARACTER3D_CLASS_ID))
        agent->bound_character = NULL;
    if (agent->bound_node && !rt_g3d_has_class(agent->bound_node, RT_G3D_SCENENODE3D_CLASS_ID))
        agent->bound_node = NULL;
    if (agent->bound_character) {
        void *pos = rt_character3d_get_position(agent->bound_character);
        navagent_vec_set(agent->position,
                         pos ? rt_vec3_x(pos) : 0.0,
                         pos ? rt_vec3_y(pos) : 0.0,
                         pos ? rt_vec3_z(pos) : 0.0);
        navagent_release_local(pos);
    } else if (agent->bound_node) {
        navagent_get_node_world_position(agent->bound_node, agent->position);
    }
    /* The synced position may have moved the agent to a new cell — keep the spatial grid aligned so
     * other agents' avoidance queries find it. (An agent only moves during its own update/sync.) */
    navagent_grid_refresh(agent);
}

/// @brief Propagate the agent's current position *out* to every live binding — the
/// controller via `rt_character3d_set_position`, the scene node via
/// `navagent_set_node_world_position`. Used when the agent is warped, or when `_update`
/// finishes and needs to keep a passive visual rig in sync with the AI mover.
/// @param agent Mutable agent whose cached position is published to valid bindings.
static void navagent_push_position_to_bindings(rt_navagent3d *agent) {
    if (!agent)
        return;
    if (agent->bound_character &&
        !rt_g3d_has_class(agent->bound_character, RT_G3D_CHARACTER3D_CLASS_ID))
        agent->bound_character = NULL;
    if (agent->bound_node && !rt_g3d_has_class(agent->bound_node, RT_G3D_SCENENODE3D_CLASS_ID))
        agent->bound_node = NULL;
    if (agent->bound_character) {
        rt_character3d_set_position(
            agent->bound_character, agent->position[0], agent->position[1], agent->position[2]);
    }
    if (agent->bound_node) {
        navagent_set_node_world_position(agent->bound_node, agent->position);
    }
}

/// @brief Advance `path_index` past any corners the agent has already reached (within
/// the corner tolerance derived from its radius). Skips zero or more consecutive corners
/// in one call so a character moving fast enough to overshoot several waypoints in a
/// single tick still progresses cleanly. Always leaves `path_index` at the final corner
/// at the latest (so the goal is never popped off the chase list).
/// @param agent Mutable agent whose active path cursor is advanced.
static void navagent_refresh_path_index(rt_navagent3d *agent) {
    double tolerance;
    if (!agent || !agent->has_path || agent->path_point_count <= 0)
        return;
    tolerance = navagent_corner_tolerance(agent);
    while (agent->path_index < agent->path_point_count - 1 &&
           navagent_dist(agent->position, navagent_path_point(agent, agent->path_index)) <=
               tolerance) {
        agent->path_index++;
    }
}

/// @brief Sum the remaining world-space path length from the agent's current position
/// to the goal — that is, the straight-line hop to the current `path_index` waypoint
/// plus every subsequent corner-to-corner segment. Clamped `path_index` into valid
/// range defensively. Callers expose this via `get_remaining_distance` so AI scripts
/// can decide "am I almost there?" without repeating the walk themselves.
/// @param agent Borrowed agent whose current path and position are measured.
/// @return Remaining polyline length capped to the navigation distance bound, or zero without an
///         active path.
static double navagent_compute_remaining_distance(rt_navagent3d *agent) {
    double remaining = 0.0;
    int32_t idx;
    if (!agent || !agent->has_path || agent->path_point_count <= 0)
        return 0.0;
    idx = agent->path_index;
    if (idx < 0)
        idx = 0;
    if (idx >= agent->path_point_count)
        idx = agent->path_point_count - 1;
    remaining += navagent_dist(agent->position, navagent_path_point(agent, idx));
    if (remaining > NAVAGENT_DISTANCE_MAX)
        return NAVAGENT_DISTANCE_MAX;
    for (int32_t i = idx; i < agent->path_point_count - 1; i++) {
        remaining +=
            navagent_dist(navagent_path_point(agent, i), navagent_path_point(agent, i + 1));
        if (remaining > NAVAGENT_DISTANCE_MAX)
            return NAVAGENT_DISTANCE_MAX;
    }
    return remaining;
}

/// @brief Clear any previous path and query the navmesh for a fresh corner list between
/// the agent's (navmesh-snapped) current position and its (navmesh-snapped) target.
/// Resets the repath timer. On empty or failed path result, zeroes velocity so the agent
/// stops cleanly instead of following a stale path. The first corner (`path_index = 1`)
/// is the next waypoint, because corner 0 is the starting position.
/// @param agent Mutable agent whose owned path buffer and path-following state are replaced.
static void navagent_rebuild_path(rt_navagent3d *agent) {
    double start[3];
    double goal[3];
    double *points = NULL;
    int64_t point_count;
    if (!agent)
        return;
    navagent_clear_path(agent);
    agent->repath_accum = 0.0;
    if (agent->navmesh && !rt_g3d_has_class(agent->navmesh, RT_G3D_NAVMESH3D_CLASS_ID))
        agent->navmesh = NULL;
    if (!agent->navmesh || !agent->has_target) {
        navagent_zero_motion(agent);
        return;
    }

    navagent_sync_position_from_bindings(agent);
    navagent_sample_point(agent, agent->position, start);
    navagent_sample_point(agent, agent->target, goal);
    navagent_vec_copy(agent->target, goal);

    void *start_vec = rt_vec3_new(start[0], start[1], start[2]);
    void *goal_vec = rt_vec3_new(goal[0], goal[1], goal[2]);
    point_count = (start_vec && goal_vec)
                      ? rt_navmesh3d_copy_path_points(agent->navmesh, start_vec, goal_vec, &points)
                      : 0;
    navagent_release_local(start_vec);
    navagent_release_local(goal_vec);
    if (point_count <= 0 || !points) {
        navagent_zero_motion(agent);
        return;
    }
    if (point_count > INT32_MAX) {
        free(points);
        navagent_zero_motion(agent);
        return;
    }
    for (int64_t i = 0; i < point_count; ++i) {
        points[i * 3 + 0] = navagent_coord_or(points[i * 3 + 0], 0.0);
        points[i * 3 + 1] = navagent_coord_or(points[i * 3 + 1], 0.0);
        points[i * 3 + 2] = navagent_coord_or(points[i * 3 + 2], 0.0);
    }

    agent->path_points_xyz = points;
    agent->path_point_count = (int32_t)point_count;
    agent->path_index = point_count > 1 ? 1 : 0;
    agent->has_path = 1;
    navagent_refresh_path_index(agent);
    agent->remaining_distance = navagent_compute_remaining_distance(agent);
}

/// @brief GC finalizer for NavAgent3D. Frees the heap-allocated path-points buffer and
/// drops references to the navmesh and any bound character / scene-node. Binding
/// references may or may not actually free on release depending on external retains.
/// @param obj Owned NavAgent3D instance being finalized; null is a no-op.
static void navagent_finalize(void *obj) {
    rt_navagent3d *agent = (rt_navagent3d *)obj;
    if (!agent)
        return;
    navagent_unregister(agent);
    free(agent->path_points_xyz);
    agent->path_points_xyz = NULL;
    free(agent->avoidance_neighbors);
    agent->avoidance_neighbors = NULL;
    agent->avoidance_neighbor_capacity = 0;
    navagent_release_class_ref(&agent->navmesh, RT_G3D_NAVMESH3D_CLASS_ID);
    navagent_release_class_ref(&agent->bound_character, RT_G3D_CHARACTER3D_CLASS_ID);
    navagent_release_class_ref(&agent->bound_node, RT_G3D_SCENENODE3D_CLASS_ID);
}

/// @brief Create a navigation agent that pathfinds across `navmesh`. `radius` and `height`
/// describe the agent's collision capsule (defaults 0.4 / 1.8 if 0). Defaults: stopping_distance
/// = radius, desired_speed = 4 u/s, auto-repath every 0.25 s, position at origin (snapped to
/// the navmesh on construction).
/// @param navmesh Optional borrowed NavMesh3D handle retained by the new agent.
/// @param radius Requested non-negative capsule and default avoidance radius; zero selects 0.4.
/// @param height Requested non-negative capsule height; zero selects 1.8.
/// @return Newly allocated GC-managed NavAgent3D, or null for an invalid navmesh or allocation
///         failure.
void *rt_navagent3d_new(void *navmesh, double radius, double height) {
    if (navmesh && !rt_g3d_has_class(navmesh, RT_G3D_NAVMESH3D_CLASS_ID))
        return NULL;
    rt_navagent3d *agent =
        (rt_navagent3d *)rt_obj_new_i64(RT_G3D_NAVAGENT3D_CLASS_ID, (int64_t)sizeof(rt_navagent3d));
    if (!agent)
        return NULL;
    memset(agent, 0, sizeof(*agent));
    agent->vptr = NULL;
    agent->radius = navagent_nonnegative_capped_or(radius, 0.4, NAVAGENT_RADIUS_MAX);
    if (agent->radius <= 0.0)
        agent->radius = 0.4;
    agent->height = navagent_nonnegative_capped_or(height, 1.8, NAVAGENT_HEIGHT_MAX);
    if (agent->height <= 0.0)
        agent->height = 1.8;
    agent->avoidance_radius = agent->radius;
    agent->stopping_distance = agent->radius > 0.0 ? agent->radius : 0.25;
    agent->desired_speed = 4.0;
    agent->repath_interval = 0.25;
    agent->auto_repath = 1;
    agent->stable_order = ++g_navagent3d_next_stable_order;
    if (agent->stable_order == 0)
        agent->stable_order = ++g_navagent3d_next_stable_order;
    if (navmesh)
        rt_obj_retain_maybe(navmesh);
    agent->navmesh = navmesh;
    navagent_vec_set(agent->position, 0.0, 0.0, 0.0);
    navagent_zero_motion(agent);
    if (agent->navmesh)
        navagent_sample_point(agent, agent->position, agent->position);
    rt_obj_set_finalizer(agent, navagent_finalize);
    navagent_register(agent);
    return agent;
}

/// @brief Set the agent's destination. The position is snapped onto the navmesh, and a path is
/// rebuilt immediately. Subsequent `_update` calls steer the agent along the path until it
/// reaches `_get_stopping_distance` of the target.
/// @param obj Opaque NavAgent3D handle.
/// @param position Borrowed Vec3 destination.
void rt_navagent3d_set_target(void *obj, void *position) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent || !rt_g3d_is_vec3(position))
        return;
    navagent_sync_position_from_bindings(agent);
    navagent_sample_point(
        agent,
        (double[3]){rt_vec3_x(position), rt_vec3_y(position), rt_vec3_z(position)},
        agent->target);
    agent->has_target = 1;
    navagent_rebuild_path(agent);
}

/// @brief Cancel the active target and clear the path. Subsequent `_update` calls do nothing
/// until a new target is set. Velocity is zeroed so AI motion stops cleanly.
/// @param obj Opaque NavAgent3D handle.
void rt_navagent3d_clear_target(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent)
        return;
    agent->has_target = 0;
    navagent_clear_path(agent);
    navagent_zero_motion(agent);
}

/// @brief Prepare one agent's path-following state without solving avoidance or moving it.
/// @details This is the snapshot phase shared by individual and batch updates. It synchronizes
///   bindings, performs any due repath, advances path corners, and publishes the preferred velocity
///   for the current start-of-tick position. No peer state is read and no position integration is
///   performed, so every member of a batch can complete this phase before avoidance starts.
/// @param agent Valid NavAgent3D instance to prepare.
/// @param dt Sanitized positive tick duration.
/// @param out_previous_position Receives the start-of-tick position for velocity reconstruction.
/// @param out_stopping_distance Receives the sanitized stopping distance used during apply.
/// @return 1 when the agent has motion to solve/apply, or 0 after publishing a stopped state.
static int navagent_prepare_update(rt_navagent3d *agent,
                                   double dt,
                                   double out_previous_position[3],
                                   double *out_stopping_distance) {
    double target_dist;
    double stopping_distance;
    if (!agent || !out_previous_position || !out_stopping_distance || !(dt > 0.0))
        return 0;

    navagent_sync_position_from_bindings(agent);
    navagent_vec_copy(out_previous_position, agent->position);
    stopping_distance =
        navagent_nonnegative_capped_or(agent->stopping_distance, 0.0, NAVAGENT_DISTANCE_MAX);
    *out_stopping_distance = stopping_distance;
    target_dist = agent->has_target ? navagent_dist(agent->position, agent->target) : 0.0;

    if (agent->auto_repath && agent->has_target && target_dist > stopping_distance) {
        if (agent->repath_accum > NAVAGENT_DISTANCE_MAX - dt)
            agent->repath_accum = agent->repath_interval;
        else
            agent->repath_accum += dt;
        if (agent->repath_accum >= agent->repath_interval) {
            navagent_rebuild_path(agent);
        }
    }

    navagent_refresh_path_index(agent);
    agent->remaining_distance = navagent_compute_remaining_distance(agent);
    if (!agent->has_path || target_dist <= stopping_distance ||
        agent->remaining_distance <= stopping_distance) {
        agent->has_path = 0;
        agent->remaining_distance = 0.0;
        navagent_zero_motion(agent);
        if (agent->bound_character && agent->bound_node) {
            navagent_push_position_to_bindings(agent);
        }
        return 0;
    }

    {
        const double *next_point = navagent_path_point(agent, agent->path_index);
        double delta[3] = {next_point[0] - agent->position[0],
                           next_point[1] - agent->position[1],
                           next_point[2] - agent->position[2]};
        double dist = navagent_len(delta);
        double speed =
            navagent_nonnegative_capped_or(agent->desired_speed, 0.0, NAVAGENT_SPEED_MAX);
        if (dist <= 1e-9) {
            navagent_refresh_path_index(agent);
            navagent_zero_motion(agent);
            agent->remaining_distance = navagent_compute_remaining_distance(agent);
            return 0;
        }
        if (!isfinite(speed) || speed < 0.0)
            speed = 0.0;
        if (dt > 0.0) {
            double clamp_speed = dist / dt;
            if (clamp_speed < speed)
                speed = clamp_speed;
        }

        agent->desired_velocity[0] = delta[0] / dist * speed;
        agent->desired_velocity[1] = delta[1] / dist * speed;
        agent->desired_velocity[2] = delta[2] / dist * speed;
    }
    return 1;
}

/// @brief Apply one already-solved desired velocity and finish post-move bookkeeping.
/// @details Character-bound agents delegate motion to the controller; unbound agents integrate
///   directly and snap to the navmesh. Actual velocity, path progress, arrival state, bindings, and
///   the live avoidance grid are then updated from the resulting position.
/// @param agent Prepared NavAgent3D whose desired velocity is final for this tick.
/// @param dt Sanitized positive tick duration.
/// @param previous_position Start-of-tick position captured by `navagent_prepare_update`.
/// @param stopping_distance Sanitized stopping distance captured during preparation.
static void navagent_apply_prepared_update(rt_navagent3d *agent,
                                           double dt,
                                           const double previous_position[3],
                                           double stopping_distance) {
    if (!agent || !previous_position || !(dt > 0.0))
        return;

    if (agent->bound_character) {
        void *move_vec = rt_vec3_new(
            agent->desired_velocity[0], agent->desired_velocity[1], agent->desired_velocity[2]);
        rt_character3d_move(agent->bound_character, move_vec, dt);
        navagent_release_local(move_vec);
        navagent_sync_position_from_bindings(agent);
        if (agent->bound_node)
            navagent_set_node_world_position(agent->bound_node, agent->position);
    } else {
        agent->position[0] += agent->desired_velocity[0] * dt;
        agent->position[1] += agent->desired_velocity[1] * dt;
        agent->position[2] += agent->desired_velocity[2] * dt;
        if (agent->navmesh)
            navagent_sample_point(agent, agent->position, agent->position);
        if (agent->bound_node)
            navagent_set_node_world_position(agent->bound_node, agent->position);
    }

    agent->velocity[0] = navagent_clamp_abs_or(
        (agent->position[0] - previous_position[0]) / dt, 0.0, NAVAGENT_SPEED_MAX);
    agent->velocity[1] = navagent_clamp_abs_or(
        (agent->position[1] - previous_position[1]) / dt, 0.0, NAVAGENT_SPEED_MAX);
    agent->velocity[2] = navagent_clamp_abs_or(
        (agent->position[2] - previous_position[2]) / dt, 0.0, NAVAGENT_SPEED_MAX);
    navagent_grid_refresh(agent);
    navagent_refresh_path_index(agent);
    agent->remaining_distance = navagent_compute_remaining_distance(agent);
    if (agent->has_target && navagent_dist(agent->position, agent->target) <= stopping_distance) {
        agent->has_path = 0;
        agent->remaining_distance = 0.0;
        navagent_zero_motion(agent);
    }
}

/// @brief Per-frame steering tick. Optionally re-paths if the auto-repath interval has elapsed,
/// computes a desired velocity toward the next path waypoint, then either moves a bound
/// CharacterController3D (if any) or integrates position directly. Falls back to snapping to the
/// navmesh after each step. Stops cleanly when within `stopping_distance` of the target.
/// @param obj Opaque NavAgent3D handle.
/// @param dt Frame duration in seconds, sanitized to `[0, NAVAGENT_DT_MAX]`.
void rt_navagent3d_update(void *obj, double dt) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    double previous_position[3];
    double stopping_distance = 0.0;
    if (!agent)
        return;
    dt = navagent_nonnegative_capped_or(dt, 0.0, NAVAGENT_DT_MAX);
    if (dt <= 0.0 || !navagent_prepare_update(agent, dt, previous_position, &stopping_distance))
        return;
    navagent_apply_local_avoidance(agent, dt);
    navagent_apply_prepared_update(agent, dt, previous_position, stopping_distance);
}

/// @brief Update an arbitrary set of agents through deterministic snapshot/solve/apply phases.
/// @details Valid unique handles are canonicalized by monotonic creation order, so caller array
///   order cannot affect preparation or publication. All selected agents first prepare their path
///   velocity; the complete live registry is then snapshotted once; every avoidance solve reads
///   only that immutable state; solved velocities and positions are finally published in the same
///   stable handle order. Invalid handles and duplicates are ignored. The function is main-thread
///   only, matching agent registration, binding, and the live spatial grid. Reusable process-owned
///   scratch buffers avoid allocation after the largest observed batch has warmed up.
/// @param agents Array of candidate NavAgent3D handles. The array is borrowed for the call.
/// @param agent_count Number of entries available in @p agents.
/// @param dt Tick duration in seconds, sanitized and capped identically to individual Update.
/// @return Number of unique valid agents processed, or zero for invalid input, non-positive time,
///   or staging allocation failure. No agent is prepared before all required staging is reserved.
int64_t rt_navagent3d_update_batch(void *const *agents, int64_t agent_count, double dt) {
    navagent_batch_context_t context;
    int32_t selected_count = 0;
    int32_t registry_count;
    RT_ASSERT_MAIN_THREAD();
    dt = navagent_nonnegative_capped_or(dt, 0.0, NAVAGENT_DT_MAX);
    if (!agents || agent_count <= 0 || agent_count > INT32_MAX || dt <= 0.0 ||
        g_navagent3d_registry_count < 0 || g_navagent3d_registry_count > INT32_MAX ||
        g_navagent_batch_active)
        return 0;
    registry_count = (int32_t)g_navagent3d_registry_count;
    if (!navagent_batch_reserve_items((int32_t)agent_count) ||
        !navagent_batch_reserve_snapshots(registry_count) ||
        !navagent_batch_reserve_neighbors(registry_count))
        return 0;

    for (int64_t i = 0; i < agent_count; i++) {
        rt_navagent3d *agent = navagent3d_checked(agents[i]);
        if (!agent)
            continue;
        navagent_batch_item_t *item = &g_navagent_batch_items[selected_count++];
        memset(item, 0, sizeof(*item));
        item->agent = agent;
        item->stable_order = agent->stable_order;
        item->snapshot_index = -1;
    }
    if (selected_count <= 0)
        return 0;
    qsort(g_navagent_batch_items,
          (size_t)selected_count,
          sizeof(*g_navagent_batch_items),
          navagent_batch_item_compare);
    int32_t unique_count = 0;
    for (int32_t i = 0; i < selected_count; i++) {
        if (unique_count > 0 &&
            g_navagent_batch_items[unique_count - 1].agent == g_navagent_batch_items[i].agent)
            continue;
        if (unique_count != i)
            g_navagent_batch_items[unique_count] = g_navagent_batch_items[i];
        unique_count++;
    }

    g_navagent_batch_active = 1;
    for (int32_t i = 0; i < unique_count; i++) {
        navagent_batch_item_t *item = &g_navagent_batch_items[i];
        item->should_apply = navagent_prepare_update(
                                 item->agent, dt, item->previous_position, &item->stopping_distance)
                                 ? 1
                                 : 0;
    }
    memset(&context, 0, sizeof(context));
    if (!navagent_batch_build_snapshot(&context)) {
        g_navagent_batch_active = 0;
        return 0;
    }
    for (int32_t i = 0; i < unique_count; i++) {
        navagent_batch_item_t *item = &g_navagent_batch_items[i];
        item->snapshot_index =
            navagent_batch_find_snapshot(context.snapshots, context.snapshot_count, item->agent);
        if (!item->should_apply)
            continue;
        if (item->snapshot_index < 0 ||
            !navagent_batch_solve_velocity(
                &context, item->snapshot_index, dt, item->solved_velocity)) {
            navagent_vec_copy(item->solved_velocity, item->agent->desired_velocity);
        }
    }

    /* Publish every solve result before any position changes, then integrate in the same stable
     * order. This keeps observers and binding side effects deterministic as well as the math. */
    for (int32_t i = 0; i < unique_count; i++) {
        navagent_batch_item_t *item = &g_navagent_batch_items[i];
        if (item->should_apply)
            navagent_vec_copy(item->agent->desired_velocity, item->solved_velocity);
    }
    for (int32_t i = 0; i < unique_count; i++) {
        navagent_batch_item_t *item = &g_navagent_batch_items[i];
        if (item->should_apply) {
            navagent_apply_prepared_update(
                item->agent, dt, item->previous_position, item->stopping_distance);
        }
    }
    g_navagent_batch_active = 0;
    return unique_count;
}

/// @brief Teleport the agent to `position` (snapped onto the navmesh). Pushes the new position
/// to any bound character/node, zeros velocity, clears the cached path, and rebuilds the path
/// if a target is still active.
/// @param obj Opaque NavAgent3D handle.
/// @param position Borrowed Vec3 destination.
void rt_navagent3d_warp(void *obj, void *position) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    double world[3];
    if (!agent || !rt_g3d_is_vec3(position))
        return;
    world[0] = rt_vec3_x(position);
    world[1] = rt_vec3_y(position);
    world[2] = rt_vec3_z(position);
    navagent_sample_point(agent, world, agent->position);
    navagent_grid_refresh(agent); /* a warp can jump cells — realign the spatial grid immediately */
    navagent_push_position_to_bindings(agent);
    navagent_zero_motion(agent);
    navagent_clear_path(agent);
    if (agent->has_target)
        navagent_rebuild_path(agent);
}

/// @brief Read the agent's current world position. Re-syncs from any bound character/node first.
/// @param obj Opaque NavAgent3D handle.
/// @return Newly allocated Vec3 owned by the caller; invalid handles produce the origin.
void *rt_navagent3d_get_position(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    double position[3];
    if (!agent)
        return rt_vec3_new(0.0, 0.0, 0.0);
    navagent_sync_position_from_bindings(agent);
    navagent_vec_copy(position, agent->position);
    return rt_vec3_new(position[0], position[1], position[2]);
}

/// @brief Read the agent's actual velocity (position-delta over the last `_update`'s dt).
/// @param obj Opaque NavAgent3D handle.
/// @return Newly allocated bounded Vec3 owned by the caller; invalid handles produce zero.
void *rt_navagent3d_get_velocity(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(navagent_clamp_abs_or(agent->velocity[0], 0.0, NAVAGENT_SPEED_MAX),
                       navagent_clamp_abs_or(agent->velocity[1], 0.0, NAVAGENT_SPEED_MAX),
                       navagent_clamp_abs_or(agent->velocity[2], 0.0, NAVAGENT_SPEED_MAX));
}

/// @brief Read the agent's *desired* velocity — the steering direction it tried to move at this
/// frame, before character-controller collisions. May differ from `_get_velocity` when blocked.
/// @param obj Opaque NavAgent3D handle.
/// @return Newly allocated bounded Vec3 owned by the caller; invalid handles produce zero.
void *rt_navagent3d_get_desired_velocity(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(navagent_clamp_abs_or(agent->desired_velocity[0], 0.0, NAVAGENT_SPEED_MAX),
                       navagent_clamp_abs_or(agent->desired_velocity[1], 0.0, NAVAGENT_SPEED_MAX),
                       navagent_clamp_abs_or(agent->desired_velocity[2], 0.0, NAVAGENT_SPEED_MAX));
}

/// @brief Returns 1 if the agent currently has an active path being followed.
/// @param obj Opaque NavAgent3D handle.
/// @return 1 for an active path, otherwise 0, including invalid handles.
int8_t rt_navagent3d_get_has_path(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    return agent && agent->has_path ? 1 : 0;
}

/// @brief World-space distance from the agent's current position along the path to the goal.
/// Updated each `_update` tick. Returns 0 when the agent has no path or is within stopping
/// distance (use `_get_has_path` to tell those apart), and -1 for an invalid agent handle.
/// @param obj Opaque NavAgent3D handle.
/// @return Bounded non-negative remaining path length, zero without a path, or -1 for an invalid
///         handle.
double rt_navagent3d_get_remaining_distance(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    return agent ? navagent_nonnegative_capped_or(
                       agent->remaining_distance, 0.0, NAVAGENT_DISTANCE_MAX)
                 : NAVAGENT_INVALID_QUERY;
}

/// @brief Distance from the goal at which the agent stops moving (default = radius). Returns -1
/// for an invalid agent handle.
/// @param obj Opaque NavAgent3D handle.
/// @return Bounded non-negative stopping distance, or -1 for an invalid handle.
double rt_navagent3d_get_stopping_distance(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    return agent ? navagent_nonnegative_capped_or(
                       agent->stopping_distance, 0.0, NAVAGENT_DISTANCE_MAX)
                 : NAVAGENT_INVALID_QUERY;
}

/// @brief Set the stopping distance (clamped to ≥ 0). Larger values cause the agent to halt
/// further from the goal — useful for combat/stand-back behaviors.
/// @param obj Opaque NavAgent3D handle.
/// @param distance Requested stopping distance, bounded to the supported navigation range.
void rt_navagent3d_set_stopping_distance(void *obj, double distance) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent)
        return;
    agent->stopping_distance = navagent_nonnegative_capped_or(distance, 0.0, NAVAGENT_DISTANCE_MAX);
}

/// @brief Maximum movement speed in world units per second (default 4). Returns -1 for an
/// invalid agent handle.
/// @param obj Opaque NavAgent3D handle.
/// @return Bounded non-negative desired speed, or -1 for an invalid handle.
double rt_navagent3d_get_desired_speed(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    return agent ? navagent_nonnegative_capped_or(agent->desired_speed, 0.0, NAVAGENT_SPEED_MAX)
                 : NAVAGENT_INVALID_QUERY;
}

/// @brief Set max movement speed (clamped to ≥ 0). Updates take effect on the next `_update`.
/// @param obj Opaque NavAgent3D handle.
/// @param speed Requested movement speed in world units per second.
void rt_navagent3d_set_desired_speed(void *obj, double speed) {
    RT_ASSERT_MAIN_THREAD();
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent)
        return;
    agent->desired_speed = navagent_nonnegative_capped_or(speed, 0.0, NAVAGENT_SPEED_MAX);
    navagent_recompute_max_reach();
}

/// @brief Returns 1 if the agent automatically rebuilds its path on the repath interval. When
/// disabled, the path is built once per `_set_target` and never refreshed.
/// @param obj Opaque NavAgent3D handle.
/// @return 1 when automatic repathing is enabled, otherwise 0, including invalid handles.
int8_t rt_navagent3d_get_auto_repath(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    return agent && agent->auto_repath ? 1 : 0;
}

/// @brief Toggle automatic re-pathing every 0.25 s. Disable to manually control path freshness
/// (useful when target is static and rebuilds would waste CPU).
/// @param obj Opaque NavAgent3D handle.
/// @param enabled Non-zero to enable periodic path rebuilding.
void rt_navagent3d_set_auto_repath(void *obj, int8_t enabled) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent)
        return;
    agent->auto_repath = enabled ? 1 : 0;
}

/// @brief Returns 1 when same-NavMesh local separation steering is enabled.
/// @param obj Opaque NavAgent3D handle.
/// @return 1 when local avoidance is enabled, otherwise 0, including invalid handles.
int8_t rt_navagent3d_get_avoidance_enabled(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    return agent && agent->avoidance_enabled ? 1 : 0;
}

/// @brief Toggle opt-in same-NavMesh local separation steering.
/// @param obj Opaque NavAgent3D handle.
/// @param enabled Non-zero to enable reciprocal local avoidance.
void rt_navagent3d_set_avoidance_enabled(void *obj, int8_t enabled) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent)
        return;
    agent->avoidance_enabled = enabled ? 1 : 0;
}

/// @brief Radius used by local avoidance neighbor separation (defaults to the agent radius).
/// Returns -1 for an invalid agent handle.
/// @param obj Opaque NavAgent3D handle.
/// @return Bounded non-negative avoidance radius, or -1 for an invalid handle.
double rt_navagent3d_get_avoidance_radius(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    return agent ? navagent_nonnegative_capped_or(agent->avoidance_radius, 0.0, NAVAGENT_RADIUS_MAX)
                 : NAVAGENT_INVALID_QUERY;
}

/// @brief Set local avoidance radius (clamped to >= 0). A zero radius disables separation force.
/// @param obj Opaque NavAgent3D handle.
/// @param radius Requested non-negative local-avoidance radius.
void rt_navagent3d_set_avoidance_radius(void *obj, double radius) {
    RT_ASSERT_MAIN_THREAD();
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent)
        return;
    agent->avoidance_radius = navagent_nonnegative_capped_or(radius, 0.0, NAVAGENT_RADIUS_MAX);
    navagent_recompute_max_reach();
}

/// @brief Bind the agent to a CharacterController3D — `_update` will call `_move` on the
/// controller (respecting collisions) instead of moving the agent's position directly. The
/// agent's position is then read back from the controller post-move. Replaces any prior binding.
/// @param obj Opaque NavAgent3D handle.
/// @param controller Optional CharacterController3D handle to retain; null clears the binding.
void rt_navagent3d_bind_character(void *obj, void *controller) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent)
        return;
    if (controller && !rt_g3d_has_class(controller, RT_G3D_CHARACTER3D_CLASS_ID))
        return;
    if (agent->bound_character == controller)
        return;
    if (controller)
        rt_obj_retain_maybe(controller);
    navagent_release_class_ref(&agent->bound_character, RT_G3D_CHARACTER3D_CLASS_ID);
    agent->bound_character = controller;
    navagent_sync_position_from_bindings(agent);
    if (agent->bound_character && agent->bound_node)
        navagent_set_node_world_position(agent->bound_node, agent->position);
}

/// @brief Bind the agent to a SceneNode3D — the node's world position will be updated to
/// match the agent each `_update`. Useful for keeping a visual rig in sync with the AI mover.
/// @param obj Opaque NavAgent3D handle.
/// @param node Optional SceneNode3D handle to retain; null clears the binding.
void rt_navagent3d_bind_node(void *obj, void *node) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent)
        return;
    if (node && !rt_g3d_has_class(node, RT_G3D_SCENENODE3D_CLASS_ID))
        return;
    if (agent->bound_node == node)
        return;
    if (node)
        rt_obj_retain_maybe(node);
    navagent_release_class_ref(&agent->bound_node, RT_G3D_SCENENODE3D_CLASS_ID);
    agent->bound_node = node;
    if (agent->bound_character) {
        navagent_sync_position_from_bindings(agent);
        if (agent->bound_node)
            navagent_set_node_world_position(agent->bound_node, agent->position);
    } else {
        navagent_sync_position_from_bindings(agent);
    }
}

/// @brief True while the agent's current path segment traverses a registered
///        off-mesh link (jump/drop/vent). Lets gameplay trigger the matching
///        traversal animation exactly when the agent commits to the link.
/// @param obj Opaque NavAgent3D handle.
/// @return 1 when the current path segment matches an off-mesh link, otherwise 0.
int8_t rt_navagent3d_get_on_offmesh_link(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    if (!agent || !agent->has_path || !agent->navmesh || !agent->path_points_xyz)
        return 0;
    if (agent->path_index < 1 || agent->path_index >= agent->path_point_count)
        return 0;
    const double *a = agent->path_points_xyz + (size_t)(agent->path_index - 1) * 3u;
    const double *b = agent->path_points_xyz + (size_t)agent->path_index * 3u;
    return rt_navmesh3d_segment_is_offmesh_link(agent->navmesh, a, b, NULL);
}

/// @brief The authored kind string of the off-mesh link being traversed
///        (empty when not on a link or the link has no kind).
/// @param obj Opaque NavAgent3D handle.
/// @return Retained runtime string for the current link kind, or the shared empty string when no
///         matching kind exists.
rt_string rt_navagent3d_get_link_kind(void *obj) {
    rt_navagent3d *agent = navagent3d_checked(obj);
    rt_string kind = NULL;
    if (!agent || !agent->has_path || !agent->navmesh || !agent->path_points_xyz)
        return rt_str_empty();
    if (agent->path_index < 1 || agent->path_index >= agent->path_point_count)
        return rt_str_empty();
    const double *a = agent->path_points_xyz + (size_t)(agent->path_index - 1) * 3u;
    const double *b = agent->path_points_xyz + (size_t)agent->path_index * 3u;
    if (!rt_navmesh3d_segment_is_offmesh_link(agent->navmesh, a, b, &kind) || !kind)
        return rt_str_empty();
    return rt_string_ref(kind);
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
