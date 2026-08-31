//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_physics3d_solver.c
// Purpose: Warm-started sequential-impulse contact solver and hierarchical
//   broad phase for the Physics3D runtime — union-find contact islands, warm
//   starting from previous-frame impulses, Gauss-Seidel velocity solve, split
//   positional (Baumgarte) correction, sleep management, and per-step contact
//   detection. Split out of rt_physics3d.c; shares types via the internal hdr.
//
// Key invariants:
//   - Contacts are detected once per substep, then solved iteratively.
//   - Per-manifold-point impulses are accumulated and clamped non-negative;
//     warm starts seed them from the matching previous-frame contact.
//   - Broad phase builds a deterministic median-split AABB hierarchy in reusable world scratch.
//
// Ownership/Lifetime:
//   - World-owned scratch buffers are reserved transactionally and reused across steps.
//   - Solver contacts borrow body/collider state only for the committed step.
//
// Links: rt_physics3d_internal.h, rt_physics3d.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic Physics3D contact detection and island-based solving.
///
/// The solver assembles hierarchical broadphase contact manifolds, partitions active bodies into
/// disjoint islands, warm-starts accumulated impulses, performs velocity and positional
/// iterations, and updates published impulses and sleep state. Large independent island
/// sets may run on a lazily owned worker pool without sharing writable body state.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_collider3d.h"
#include "rt_game3d_diagnostics.h"
#include "rt_graphics3d_ids.h"
#include "rt_physics3d.h"
#include "rt_physics3d_internal.h"
#include "rt_trap.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PH3D_BROADPHASE_STACK_FALLBACK 128

/*======================================================================
 * Parallel island dispatch. Islands are disjoint body/contact sets, so
 * running them on worker threads produces bit-identical results to the
 * serial island loop regardless of scheduling. The pool is owned by the
 * world (created lazily, released by the world finalizer).
 *======================================================================*/

/// @brief Creates a runtime thread pool with a requested worker count.
/// @param size Positive number of worker threads to create.
/// @return Owned pool handle, or null when creation fails.
extern void *rt_threadpool_new(int64_t size);

/// @brief Enqueues a native callback and opaque argument on a runtime thread pool.
/// @param pool Borrowed thread-pool handle.
/// @param callback Function invoked once with @p arg by a worker.
/// @param arg Opaque borrowed callback argument that must remain valid until the pool is drained.
/// @return Nonzero when the task was accepted; zero when submission fails.
extern int8_t rt_threadpool_submit_fn(void *pool, void (*callback)(void *), void *arg);

/// @brief Waits until all tasks submitted to a runtime thread pool have completed.
/// @param pool Borrowed thread-pool handle.
extern void rt_threadpool_wait(void *pool);

/// @brief Stops a runtime thread pool and releases its owned resources.
/// @param pool Owned thread-pool handle to shut down.
extern void rt_threadpool_shutdown(void *pool);

/// @brief Returns the runtime's platform-adjusted default parallel worker count.
/// @return Recommended positive worker count.
extern int64_t rt_parallel_default_workers(void);

#define PH3D_SOLVER_MAX_WORKERS 8
/* Below this many solvable contacts the dispatch overhead beats the win. */
#define PH3D_SOLVER_PARALLEL_MIN_CONTACTS 64

typedef struct {
    rt_world3d *w;
    const ph3d_solver_island_batch *batch;
    int32_t island_begin;
    int32_t island_end;
    int solve_position; /* 0 = velocity pass, 1 = position pass */
    double beta;
} ph3d_island_task_t;

static void world3d_solve_velocity_contact(rt_contact3d *c);
static void world3d_solve_position_contact(rt_contact3d *c, double beta);

/// @brief Executes one contiguous solver-island task on the current thread.
/// @param arg Borrowed pointer to a `ph3d_island_task_t` describing the world, island range,
///        solver pass, and positional beta. Invalid task state is a no-op.
static void ph3d_island_task_run(void *arg) {
    ph3d_island_task_t *task = (ph3d_island_task_t *)arg;
    if (!task || !task->w || !task->batch)
        return;
    for (int32_t island = task->island_begin; island < task->island_end; ++island) {
        int32_t begin = task->batch->island_offsets[island];
        int32_t end = task->batch->island_offsets[island + 1];
        for (int32_t i = begin; i < end; ++i) {
            rt_contact3d *c = &task->w->contacts[task->batch->contact_indices[i]];
            if (task->solve_position)
                world3d_solve_position_contact(c, task->beta);
            else
                world3d_solve_velocity_contact(c);
        }
    }
}

/// @brief Lazily creates the world's bounded solver worker pool.
/// @param w Borrowed world that owns the pool and its permanent failure flag.
/// @return Borrowed pool handle, or null when @p w is invalid, fewer than two workers are
///         available, or pool creation previously or currently failed.
static void *world3d_solver_pool(rt_world3d *w) {
    int64_t workers;
    if (!w)
        return NULL;
    if (w->solver_pool || w->solver_pool_failed)
        return w->solver_pool;
    workers = rt_parallel_default_workers();
    if (workers > PH3D_SOLVER_MAX_WORKERS)
        workers = PH3D_SOLVER_MAX_WORKERS;
    if (workers < 2) {
        w->solver_pool_failed = 1;
        return NULL;
    }
    w->solver_pool = rt_threadpool_new(workers);
    if (!w->solver_pool)
        w->solver_pool_failed = 1;
    return w->solver_pool;
}

/// @brief Dispatch one solver pass over the islands, in parallel when profitable.
/// @details Chunks islands contiguously across workers (deterministic assignment;
///   result identical to serial since islands share no state). Any failed
///   submission runs its chunk inline so every island is solved exactly once.
/// @param w Borrowed world containing contact manifolds and owning the optional worker pool.
/// @param batch Borrowed island layout whose offsets and contact indices select the work.
/// @param solve_position Nonzero for positional correction; zero for velocity impulses.
/// @param beta Baumgarte positional correction factor, ignored by the velocity pass.
static void world3d_solve_islands_dispatch(rt_world3d *w,
                                           const ph3d_solver_island_batch *batch,
                                           int solve_position,
                                           double beta) {
    void *pool = NULL;
    int32_t chunks;
    ph3d_island_task_t tasks[PH3D_SOLVER_MAX_WORKERS];
    if (!w || !batch || batch->island_count <= 0)
        return;
    if (batch->island_count >= 2 &&
        batch->solver_contact_count >= PH3D_SOLVER_PARALLEL_MIN_CONTACTS)
        pool = world3d_solver_pool(w);
    if (!pool) {
        ph3d_island_task_t serial = {w, batch, 0, batch->island_count, solve_position, beta};
        ph3d_island_task_run(&serial);
        return;
    }
    chunks = (int32_t)rt_parallel_default_workers();
    if (chunks > PH3D_SOLVER_MAX_WORKERS)
        chunks = PH3D_SOLVER_MAX_WORKERS;
    if (chunks > batch->island_count)
        chunks = batch->island_count;
    {
        int32_t per_chunk = batch->island_count / chunks;
        int32_t extra = batch->island_count % chunks;
        int32_t island = 0;
        for (int32_t t = 0; t < chunks; ++t) {
            int32_t span = per_chunk + (t < extra ? 1 : 0);
            tasks[t].w = w;
            tasks[t].batch = batch;
            tasks[t].island_begin = island;
            tasks[t].island_end = island + span;
            tasks[t].solve_position = solve_position;
            tasks[t].beta = beta;
            island += span;
            if (!rt_threadpool_submit_fn(pool, ph3d_island_task_run, &tasks[t]))
                ph3d_island_task_run(&tasks[t]);
        }
        rt_threadpool_wait(pool);
    }
}

/* ====================================================================== *
 * Warm-started sequential-impulse contact solver (Phase 8).
 *
 * Replaces the prior per-iteration re-detect-and-resolve loop. The step now
 * detects contacts once per substep, seeds each manifold point's accumulated
 * normal/friction impulse from the matching previous-frame contact (warm
 * starting), iterates a Gauss-Seidel velocity solve with non-negative
 * accumulated-impulse clamping, then applies a split positional correction so
 * recovery never injects energy into the velocity state. This is what lets
 * box stacks and piles rest stably instead of jittering and sinking.
 * ====================================================================== */

#define PH3D_PENETRATION_SLOP 0.005
#define PH3D_MAX_POSITION_CORRECTION 0.2 /* clamp on per-step positional push (m) */
/* Carry <100% of the prior impulse so an uneven manifold (Gauss-Seidel tends to
 * load the first-solved point) cannot compound its torque frame-over-frame into
 * a slow tip-over. The solver rebuilds the remainder each step. */
#define PH3D_WARMSTART_FACTOR 0.8
#define PH3D_CONTACT_WAKE_SPEED 0.1 /* approach speed (m/s) that wakes a sleeper */

/// @brief Build two orthonormal tangents spanning the plane perpendicular to
///   unit normal @p n (deterministic basis so warm-started friction impulses
///   stay aligned frame to frame while the normal is stable).
/// @param n Borrowed three-element contact normal, expected to be normalized.
/// @param[out] t1 Three-element destination for the first normalized tangent.
/// @param[out] t2 Three-element destination for the second normalized tangent.
static void ph3d_contact_tangents(const double *n, double *t1, double *t2) {
    if (fabs(n[0]) >= 0.57735) {
        vec3_set(t1, n[1], -n[0], 0.0);
    } else {
        vec3_set(t1, 0.0, n[2], -n[1]);
    }
    if (vec3_normalize_in_place(t1) < 1e-12)
        vec3_set(t1, 1.0, 0.0, 0.0);
    vec3_cross(n, t1, t2);
    if (vec3_normalize_in_place(t2) < 1e-12) {
        /* n and t1 nearly parallel (only reachable with a degenerate, non-unit
         * normal): pick any axis orthogonal to t1 so friction never operates on a
         * zero-length tangent. No-op for the valid unit-normal case above. */
        vec3_set(t2, -t1[1], t1[0], 0.0);
        if (vec3_normalize_in_place(t2) < 1e-12)
            vec3_set(t2, 0.0, 0.0, 1.0);
    }
}

/// @brief Effective mass (1 / scalar inverse-mass) for a unit constraint @p axis
///   at the given per-body contact arms, including the angular term.
/// @param a Borrowed first contact body.
/// @param r_a Borrowed three-element arm from @p a's center of mass to contact.
/// @param b Borrowed second contact body.
/// @param r_b Borrowed three-element arm from @p b's center of mass to contact.
/// @param axis Borrowed normalized constraint axis.
/// @return Reciprocal scalar impulse denominator, or zero for an immovable or degenerate pair.
static double ph3d_contact_effective_mass(const rt_body3d *a,
                                          const double *r_a,
                                          const rt_body3d *b,
                                          const double *r_b,
                                          const double *axis) {
    double denom = body3d_contact_impulse_denominator(a, r_a, axis) +
                   body3d_contact_impulse_denominator(b, r_b, axis);
    return denom > 1e-12 ? 1.0 / denom : 0.0;
}

/// @brief Relative velocity at a contact arm pair projected onto @p axis.
/// @param a Borrowed first body.
/// @param r_a Borrowed contact arm for @p a.
/// @param b Borrowed second body.
/// @param r_b Borrowed contact arm for @p b.
/// @param axis Borrowed projection axis.
/// @return `dot(velocity_b - velocity_a, axis)` at the two contact points.
static double ph3d_contact_relative_velocity(const rt_body3d *a,
                                             const double *r_a,
                                             const rt_body3d *b,
                                             const double *r_b,
                                             const double *axis) {
    double vel_a[3];
    double vel_b[3];
    double rel[3];
    body3d_contact_velocity(a, r_a, vel_a);
    body3d_contact_velocity(b, r_b, vel_b);
    vec3_sub(vel_b, vel_a, rel);
    return vec3_dot(rel, axis);
}

/// @brief Apply impulse @p magnitude along @p dir at the contact arms (b gets +,
///   a gets -, matching the a→b normal convention).
/// @param[in,out] a Borrowed first body receiving the negative impulse.
/// @param r_a Borrowed contact arm for @p a.
/// @param[in,out] b Borrowed second body receiving the positive impulse.
/// @param r_b Borrowed contact arm for @p b.
/// @param dir Borrowed normalized impulse direction.
/// @param magnitude Signed scalar impulse. Zero is a no-op.
static void ph3d_apply_contact_axis_impulse(rt_body3d *a,
                                            const double *r_a,
                                            rt_body3d *b,
                                            const double *r_b,
                                            const double *dir,
                                            double magnitude) {
    double p[3];
    double neg[3];
    if (magnitude == 0.0)
        return;
    p[0] = magnitude * dir[0];
    p[1] = magnitude * dir[1];
    p[2] = magnitude * dir[2];
    vec3_negate(p, neg);
    body3d_apply_contact_impulse(a, neg, r_a);
    body3d_apply_contact_impulse(b, p, r_b);
}

/// @brief A body actively participates in the solve when it is dynamic and awake.
/// @param b Borrowed body to classify.
/// @return `1` when @p b is non-null, dynamic, and awake; otherwise `0`.
static int ph3d_body_is_active(const rt_body3d *b) {
    return b && b->motion_mode == PH3D_MODE_DYNAMIC && !b->is_sleeping;
}

/// @brief Whether a contact needs solving this step; also propagates wake-up.
/// @details A contact is solved when at least one body is an active (awake,
///   dynamic) body. If one side is active and the other is a *sleeping* dynamic
///   body, the sleeper is woken so the pair solves correctly (wake spreads one
///   contact layer per step). A contact between two non-active bodies (both
///   sleeping, or sleeping-vs-static) is skipped — this is what freezes a fully
///   settled island so a resting stack stays put instead of being re-solved
///   (and slowly drifting) every frame.
/// @note The one-layer-per-step wake front is a deliberate tradeoff: poking the
///   base of a tall settled stack ripples up over a few frames rather than waking
///   the whole island at once. Whole-island waking was considered and rejected —
///   it would re-solve (and let drift) bodies that should stay asleep, and it
///   changes solver trajectories (VM-vs-native determinism). The contact-driven
///   propagation here is the accepted behavior.
/// @param[in,out] a Borrowed first contact body, which may be woken.
/// @param[in,out] b Borrowed second contact body, which may be woken.
/// @return `1` when at least one body is active and the pair should be solved; otherwise `0`.
static int ph3d_contact_should_solve(rt_body3d *a, rt_body3d *b) {
    int active_a = ph3d_body_is_active(a);
    int active_b = ph3d_body_is_active(b);
    if (!active_a && !active_b)
        return 0;
    if (active_a && !active_b && b && b->motion_mode == PH3D_MODE_DYNAMIC)
        body3d_wake_if_dynamic(b);
    else if (active_b && !active_a && a && a->motion_mode == PH3D_MODE_DYNAMIC)
        body3d_wake_if_dynamic(a);
    return 1;
}

/// @brief Reset a solver-island batch. Safe on NULL.
/// @details The batch arrays are carved out of the world's
///   PH3D_SCRATCH_SOLVER_BATCH slot (persistent, reused every substep), so
///   there is nothing to free here — only the view is cleared.
/// @param[out] batch Batch view to clear; may be null.
void ph3d_solver_island_batch_free(ph3d_solver_island_batch *batch) {
    if (!batch)
        return;
    memset(batch, 0, sizeof(*batch));
}

/// @brief Push a non-negative @p value onto a growable int32 stack (initial capacity 128,
///   doubling thereafter), with overflow checks.
/// @param[in,out] items Address of the caller-owned allocation, updated after successful growth.
/// @param[in,out] count Number of initialized values, incremented on success.
/// @param[in,out] capacity Writable element capacity of @p items.
/// @param value Non-negative value to append.
/// @return 1 on success, 0 on invalid args, a negative value, or allocation failure.
int ph3d_i32_stack_push(int32_t **items, int32_t *count, int32_t *capacity, int32_t value) {
    int32_t new_capacity;
    int32_t *grown;
    if (!items || !count || !capacity || value < 0)
        return 0;
    if (*count >= *capacity) {
        new_capacity = *capacity < 128 ? 128 : *capacity;
        if (new_capacity > INT32_MAX / 2)
            return 0;
        if (*capacity >= 128)
            new_capacity *= 2;
        if ((size_t)new_capacity > SIZE_MAX / sizeof(**items))
            return 0;
        grown = (int32_t *)realloc(*items, (size_t)new_capacity * sizeof(**items));
        if (!grown)
            return 0;
        *items = grown;
        *capacity = new_capacity;
    }
    (*items)[(*count)++] = value;
    return 1;
}

/// @brief Carve the per-body and per-contact scratch arrays for one island-solver batch
///   (union-find parent, active-body flags, root/body→island maps, per-island contact
///   counts/write offsets, island offset table, contact-index buffer) out of the world's
///   persistent PH3D_SCRATCH_SOLVER_BATCH slot.
/// @details One zeroed block per substep replaces what used to be 8 malloc/calloc + 8 free
///   pairs per substep. The slot grows geometrically and lives until world teardown, so the
///   solver's hottest path performs no allocator round-trips once warmed.
/// @param w Borrowed world supplying body/contact counts and persistent step scratch.
/// @param[out] batch Batch whose typed array views are assigned into the acquired block.
/// @return 1 when the scratch is available, 0 on bad world state or allocation failure.
static int ph3d_solver_island_batch_alloc(rt_world3d *w, ph3d_solver_island_batch *batch) {
    size_t body_slots;
    size_t contact_slots;
    size_t total_slots;
    int32_t *mem;
    if (!w || !batch || w->body_count < 0 || w->contact_count < 0)
        return 0;
    if (w->body_count == INT32_MAX)
        return 0;
    body_slots = (size_t)w->body_count;
    contact_slots = (size_t)w->contact_count;
    /* 6 per-body arrays + island_offsets (body_slots + 1) + contact_indices. */
    if (body_slots > (SIZE_MAX / sizeof(int32_t) - 1u) / 7u)
        return 0;
    total_slots = body_slots * 7u + 1u;
    if (contact_slots > SIZE_MAX / sizeof(int32_t) - total_slots)
        return 0;
    total_slots += contact_slots;
    mem = (int32_t *)world3d_step_scratch_acquire(
        w, PH3D_SCRATCH_SOLVER_BATCH, total_slots * sizeof(int32_t), 1);
    if (!mem)
        return 0;
    batch->parent = mem;
    batch->active_body = mem + body_slots;
    batch->root_to_island = mem + body_slots * 2u;
    batch->body_island = mem + body_slots * 3u;
    batch->island_contact_counts = mem + body_slots * 4u;
    batch->island_write_offsets = mem + body_slots * 5u;
    batch->island_offsets = mem + body_slots * 6u;
    batch->contact_indices = mem + body_slots * 7u + 1u;
    return 1;
}

/// @brief Return the world's index for @p body, using cached owner metadata first.
/// @param w Borrowed world whose body array is searched.
/// @param body Borrowed body whose stable index is requested.
/// @return Index of @p body in @p w, or -1 if absent or either argument is NULL.
static int32_t world3d_body_index_of(const rt_world3d *w, const rt_body3d *body) {
    if (!w || !body)
        return -1;
    if (body->owner_world == w && body->owner_index >= 0 && body->owner_index < w->body_count &&
        w->bodies[body->owner_index] == body)
        return body->owner_index;
    for (int32_t i = 0; i < w->body_count; ++i) {
        if (w->bodies[i] == body)
            return i;
    }
    return -1;
}

/// @brief Union-find FIND with path compression: return the island root of @p index.
/// @details Walks parent links to the root, then re-points every node on the path directly at
///          the root so subsequent queries are near-constant time.
/// @param[in,out] parent Union-find parent array, updated by path compression.
/// @param index Valid starting body index.
/// @return Root index representing the connected component containing @p index.
static int32_t ph3d_island_find(int32_t *parent, int32_t index) {
    int32_t root = index;
    while (parent[root] != root)
        root = parent[root];
    while (parent[index] != index) {
        int32_t next = parent[index];
        parent[index] = root;
        index = next;
    }
    return root;
}

/// @brief Union-find UNION: merge the islands containing bodies @p a and @p b.
/// @details Roots are joined under the numerically smaller index, giving a deterministic island
///          layout independent of contact ordering. No-op when either index is negative.
/// @param[in,out] parent Union-find parent array.
/// @param a First body index, or a negative value for no body.
/// @param b Second body index, or a negative value for no body.
static void ph3d_island_union(int32_t *parent, int32_t a, int32_t b) {
    int32_t root_a;
    int32_t root_b;
    if (!parent || a < 0 || b < 0)
        return;
    root_a = ph3d_island_find(parent, a);
    root_b = ph3d_island_find(parent, b);
    if (root_a == root_b)
        return;
    if (root_b < root_a) {
        int32_t tmp = root_a;
        root_a = root_b;
        root_b = tmp;
    }
    parent[root_b] = root_a;
}

/// @brief Resolve the world-array indices of a contact's two bodies for solving.
/// @details Writes -1 to both outputs first, then skips trigger contacts and pairs where neither
///          body is active (both asleep/static) so the island builder only links solvable pairs.
/// @param w Borrowed world whose body indices are resolved.
/// @param contact Borrowed contact to classify.
/// @param[out] index_a Optional destination initialized to `-1`, then set to body A's world index.
/// @param[out] index_b Optional destination initialized to `-1`, then set to body B's world index.
/// @return 1 if the contact should be solved (indices written); 0 if it should be skipped.
static int ph3d_contact_solver_body_indices(const rt_world3d *w,
                                            const rt_contact3d *contact,
                                            int32_t *index_a,
                                            int32_t *index_b) {
    if (index_a)
        *index_a = -1;
    if (index_b)
        *index_b = -1;
    if (!w || !contact || contact->is_trigger)
        return 0;
    if (!ph3d_body_is_active(contact->body_a) && !ph3d_body_is_active(contact->body_b))
        return 0;
    if (index_a)
        *index_a = world3d_body_index_of(w, contact->body_a);
    if (index_b)
        *index_b = world3d_body_index_of(w, contact->body_b);
    return 1;
}

/// @brief Partition active bodies into solver islands and bucket each contact under its island.
/// @details Uses union-find over solvable contacts to group connected active bodies into islands,
///          assigns each island a dense id, then builds a CSR-style layout: `island_offsets` holds
///          the per-island prefix sums and `contact_indices` the contacts flattened in island
///          order. Solving islands independently decouples unrelated groups and is the prerequisite
///          for parallelizing the velocity/position solve. The batch must be released with
///          ph3d_solver_island_batch_free.
/// @param w Borrowed world whose current contacts and active bodies are partitioned.
/// @param[out] batch Caller-owned batch view, cleared first and then backed by world scratch.
/// @return 1 on success (including the empty-world no-op); 0 after trapping on allocation failure.
int world3d_build_solver_island_batch(rt_world3d *w, ph3d_solver_island_batch *batch) {
    if (!batch)
        return 0;
    memset(batch, 0, sizeof(*batch));
    if (!w || w->body_count <= 0 || w->contact_count <= 0)
        return 1;

    if (!ph3d_solver_island_batch_alloc(w, batch)) {
        ph3d_solver_island_batch_free(batch);
        world3d_step_fail(w, "Physics3D.World.Step: solver island allocation failed");
        return 0;
    }

    for (int32_t i = 0; i < w->body_count; ++i) {
        batch->parent[i] = i;
        batch->root_to_island[i] = -1;
        batch->body_island[i] = -1;
    }

    for (int32_t i = 0; i < w->contact_count; ++i) {
        rt_contact3d *contact = &w->contacts[i];
        int32_t index_a;
        int32_t index_b;
        int active_a;
        int active_b;
        if (contact->is_trigger || !ph3d_contact_should_solve(contact->body_a, contact->body_b))
            continue;
        index_a = world3d_body_index_of(w, contact->body_a);
        index_b = world3d_body_index_of(w, contact->body_b);
        active_a = index_a >= 0 && ph3d_body_is_active(contact->body_a);
        active_b = index_b >= 0 && ph3d_body_is_active(contact->body_b);
        if (active_a)
            batch->active_body[index_a] = 1;
        if (active_b)
            batch->active_body[index_b] = 1;
        if (active_a && active_b)
            ph3d_island_union(batch->parent, index_a, index_b);
    }

    for (int32_t i = 0; i < w->body_count; ++i) {
        int32_t root;
        int32_t island;
        if (!batch->active_body[i])
            continue;
        root = ph3d_island_find(batch->parent, i);
        island = batch->root_to_island[root];
        if (island < 0) {
            island = batch->island_count++;
            batch->root_to_island[root] = island;
        }
        batch->body_island[i] = island;
        batch->active_body_count++;
    }

    for (int32_t i = 0; i < w->contact_count; ++i) {
        int32_t index_a;
        int32_t index_b;
        int32_t island = -1;
        rt_contact3d *contact = &w->contacts[i];
        if (!ph3d_contact_solver_body_indices(w, contact, &index_a, &index_b))
            continue;
        if (index_a >= 0 && batch->body_island[index_a] >= 0)
            island = batch->body_island[index_a];
        else if (index_b >= 0 && batch->body_island[index_b] >= 0)
            island = batch->body_island[index_b];
        if (island < 0)
            continue;
        batch->island_contact_counts[island]++;
        batch->solver_contact_count++;
    }

    for (int32_t island = 0; island < batch->island_count; ++island)
        batch->island_offsets[island + 1] =
            batch->island_offsets[island] + batch->island_contact_counts[island];
    memcpy(batch->island_write_offsets,
           batch->island_offsets,
           sizeof(int32_t) * (size_t)batch->island_count);

    for (int32_t i = 0; i < w->contact_count; ++i) {
        int32_t index_a;
        int32_t index_b;
        int32_t island = -1;
        int32_t write_index;
        rt_contact3d *contact = &w->contacts[i];
        if (!ph3d_contact_solver_body_indices(w, contact, &index_a, &index_b))
            continue;
        if (index_a >= 0 && batch->body_island[index_a] >= 0)
            island = batch->body_island[index_a];
        else if (index_b >= 0 && batch->body_island[index_b] >= 0)
            island = batch->body_island[index_b];
        if (island < 0)
            continue;
        write_index = batch->island_write_offsets[island]++;
        batch->contact_indices[write_index] = i;
    }

    return 1;
}

/// @brief Match this frame's contacts to the previous frame, seed accumulated
///   impulses (warm starting), capture per-point restitution bias, and re-apply
///   the seeded impulse so the velocity solver starts near the converged answer.
/// @param w Borrowed world providing prior contacts and restitution configuration.
/// @param[in,out] c Current contact manifold whose accumulators and body velocities are updated.
/// @param prev_table Optional borrowed previous-contact hash table for constant-time pair lookup.
/// @param prev_capacity Number of slots in @p prev_table; ignored when the table is null.
static void world3d_warm_start_contact(rt_world3d *w,
                                       rt_contact3d *c,
                                       const contact_pair_hash_entry *prev_table,
                                       int32_t prev_capacity) {
    rt_body3d *a;
    rt_body3d *b;
    const rt_contact3d *prev = NULL;
    double e;
    if (!w || !c)
        return;
    a = c->body_a;
    b = c->body_b;
    if (!a || !b || c->is_trigger || !ph3d_contact_should_solve(a, b))
        return;
    {
        double ra = rt_collider3d_effective_restitution_raw(c->collider_a, a->restitution);
        double rb = rt_collider3d_effective_restitution_raw(c->collider_b, b->restitution);
        e = (ra < rb) ? ra : rb;
    }

    /* Identical body/collider ordering keeps stored impulse directions valid.
     * Use the prebuilt previous-frame index when available (O(1)); fall back to a
     * linear scan if the table couldn't be built. */
    if (prev_table) {
        prev = contact_pair_table_find(prev_table, prev_capacity, c);
    } else {
        for (int32_t p = 0; p < w->previous_contact_count; ++p) {
            const rt_contact3d *q = &w->previous_contacts[p];
            if (q->body_a == c->body_a && q->body_b == c->body_b &&
                q->collider_a == c->collider_a && q->collider_b == c->collider_b) {
                prev = q;
                break;
            }
        }
    }

    for (int32_t k = 0; k < c->contact_count; ++k) {
        double n[3];
        double t1[3];
        double t2[3];
        double r_a[3];
        double r_b[3];
        double seed_n = 0.0;
        double seed_t0 = 0.0;
        double seed_t1 = 0.0;
        double vn0;
        vec3_copy(n, c->normals[k]);
        ph3d_contact_tangents(n, t1, t2);

        /* Index-based match: the manifold builder emits points in a stable
         * order for a persistent pair, so current point k maps to previous
         * point k. This survives fast motion (unlike a position-proximity
         * match), which is what keeps multi-box stacks converged while they
         * settle. The normal-agreement guard prevents carrying impulse
         * across a normal flip (e.g. a deep-penetration axis change). */
        if (prev && k < prev->contact_count && vec3_dot(n, prev->normals[k]) > 0.9) {
            seed_n = prev->normal_impulse_acc[k] * PH3D_WARMSTART_FACTOR;
            seed_t0 = prev->tangent_impulse_acc[k][0] * PH3D_WARMSTART_FACTOR;
            seed_t1 = prev->tangent_impulse_acc[k][1] * PH3D_WARMSTART_FACTOR;
        }
        c->normal_impulse_acc[k] = seed_n;
        c->tangent_impulse_acc[k][0] = seed_t0;
        c->tangent_impulse_acc[k][1] = seed_t1;

        vec3_sub(c->points[k], a->position, r_a);
        vec3_sub(c->points[k], b->position, r_b);
        vn0 = ph3d_contact_relative_velocity(a, r_a, b, r_b, n);
        /* Restitution is an impact phenomenon: apply it only on the first
         * frame of a PAIR (prev == NULL). Persistence is judged at pair
         * level, not from the warm-start seed: a seed rejected for other
         * reasons (normal-agreement guard, manifold point-count change)
         * must not re-inject an impact bounce into a resting stack. */
        {
            double threshold = ph3d_clamp_nonnegative_finite(w->restitution_threshold,
                                                             PH3D_DEFAULT_RESTITUTION_THRESHOLD);
            c->restitution_bias[k] = (prev == NULL && vn0 < -threshold) ? -e * vn0 : 0.0;
        }

        ph3d_apply_contact_axis_impulse(a, r_a, b, r_b, n, seed_n);
        ph3d_apply_contact_axis_impulse(a, r_a, b, r_b, t1, seed_t0);
        ph3d_apply_contact_axis_impulse(a, r_a, b, r_b, t2, seed_t1);
    }
}

/// @brief Sequential-impulse velocity solve for one contact manifold.
/// @details For each manifold point, solves the two friction (tangent) axes first — clamping the
///          accumulated tangent impulse to the friction cone `|t| <= mu * normal_impulse` with a
///          combined `mu = sqrt(friction_a * friction_b)` — then the normal axis, clamping the
///          accumulated normal impulse to be non-negative and steering toward the stored
///          restitution bias. Impulse deltas (new minus accumulated) are applied immediately so
///          later points in the same iteration see updated velocities.
/// @param[in,out] c Contact manifold whose accumulated impulses and body velocities are updated.
static void world3d_solve_velocity_contact(rt_contact3d *c) {
    rt_body3d *a;
    rt_body3d *b;
    double mu;
    if (!c)
        return;
    a = c->body_a;
    b = c->body_b;
    if (!a || !b || c->is_trigger || (!ph3d_body_is_active(a) && !ph3d_body_is_active(b)))
        return;
    {
        /* Per-collider material overrides win over body values (plan 20);
         * the geometric-mean combine is unchanged. */
        double fa = ph3d_clamp_nonnegative_finite(
            rt_collider3d_effective_friction_raw(c->collider_a, a->friction), 0.0);
        double fb = ph3d_clamp_nonnegative_finite(
            rt_collider3d_effective_friction_raw(c->collider_b, b->friction), 0.0);
        mu = sqrt(fa) * sqrt(fb);
        if (!isfinite(mu))
            mu = 0.0;
    }
    for (int32_t k = 0; k < c->contact_count; ++k) {
        double n[3];
        double t1[3];
        double t2[3];
        double r_a[3];
        double r_b[3];
        const double *tan_axes[2];
        int axis;
        vec3_copy(n, c->normals[k]);
        ph3d_contact_tangents(n, t1, t2);
        vec3_sub(c->points[k], a->position, r_a);
        vec3_sub(c->points[k], b->position, r_b);
        tan_axes[0] = t1;
        tan_axes[1] = t2;

        {
            double max_f = mu * c->normal_impulse_acc[k];
            for (axis = 0; axis < 2; ++axis) {
                double eff = ph3d_contact_effective_mass(a, r_a, b, r_b, tan_axes[axis]);
                double vt = ph3d_contact_relative_velocity(a, r_a, b, r_b, tan_axes[axis]);
                double old = c->tangent_impulse_acc[k][axis];
                double next = old - eff * vt;
                if (next > max_f)
                    next = max_f;
                else if (next < -max_f)
                    next = -max_f;
                c->tangent_impulse_acc[k][axis] = next;
                ph3d_apply_contact_axis_impulse(a, r_a, b, r_b, tan_axes[axis], next - old);
            }
            /* Project the accumulated tangent impulse onto the circular Coulomb
             * cone. The per-axis clamps above form a SQUARE whose diagonal admits
             * sqrt(2)*mu*N total friction, making grip direction-dependent; the
             * projection restores an isotropic friction limit. */
            {
                double t0_acc = c->tangent_impulse_acc[k][0];
                double t1_acc = c->tangent_impulse_acc[k][1];
                double len_sq = t0_acc * t0_acc + t1_acc * t1_acc;
                if (len_sq > max_f * max_f && len_sq > 1e-24) {
                    double scale = max_f / sqrt(len_sq);
                    double clamped0 = t0_acc * scale;
                    double clamped1 = t1_acc * scale;
                    ph3d_apply_contact_axis_impulse(a, r_a, b, r_b, t1, clamped0 - t0_acc);
                    ph3d_apply_contact_axis_impulse(a, r_a, b, r_b, t2, clamped1 - t1_acc);
                    c->tangent_impulse_acc[k][0] = clamped0;
                    c->tangent_impulse_acc[k][1] = clamped1;
                }
            }
        }

        {
            double eff = ph3d_contact_effective_mass(a, r_a, b, r_b, n);
            double vn = ph3d_contact_relative_velocity(a, r_a, b, r_b, n);
            double old = c->normal_impulse_acc[k];
            double next = old - eff * (vn - c->restitution_bias[k]);
            if (next < 0.0)
                next = 0.0;
            c->normal_impulse_acc[k] = next;
            ph3d_apply_contact_axis_impulse(a, r_a, b, r_b, n, next - old);
        }
    }
}

/// @brief Solve one swept time-of-impact manifold with the ordinary contact material rules.
/// @details The TOI manifold is separated (zero penetration), so it needs only
///   the standard warm-start/restitution initialization and velocity iterations.
///   Reusing `world3d_solve_velocity_contact` makes collider overrides, geometric-
///   mean friction, restitution thresholds, effective mass, and angular response
///   identical to discrete contacts. No allocation or positional correction occurs.
/// @param w Borrowed world supplying solver iteration and material configuration.
/// @param contact Caller-owned single-point contact, updated with solved impulses.
void world3d_solve_toi_contact(rt_world3d *w, rt_contact3d *contact) {
    double total_impulse = 0.0;
    int32_t iterations;
    if (!w || !contact || contact->is_trigger || contact->contact_count <= 0)
        return;
    memset(contact->normal_impulse_acc, 0, sizeof(contact->normal_impulse_acc));
    memset(contact->tangent_impulse_acc, 0, sizeof(contact->tangent_impulse_acc));
    memset(contact->restitution_bias, 0, sizeof(contact->restitution_bias));
    world3d_warm_start_contact(w, contact, NULL, 0);
    iterations = w->solver_iterations;
    if (iterations < 1)
        iterations = 1;
    else if (iterations > PH3D_MAX_SOLVER_ITERATIONS)
        iterations = PH3D_MAX_SOLVER_ITERATIONS;
    for (int32_t iteration = 0; iteration < iterations; ++iteration)
        world3d_solve_velocity_contact(contact);
    for (int32_t point = 0; point < contact->contact_count; ++point)
        total_impulse += contact->normal_impulse_acc[point];
    contact->normal_impulse = isfinite(total_impulse) && total_impulse > 0.0 ? total_impulse : 0.0;
    if (contact->relative_speed > PH3D_CONTACT_WAKE_SPEED) {
        body3d_wake_if_dynamic(contact->body_a);
        body3d_wake_if_dynamic(contact->body_b);
    }
}

/* Per-pass cap on the angular part of the positional correction (radians).
 * Long lever arms on light bodies could otherwise spin a box visibly in one
 * pass; the cap keeps NGS convergence smooth (mirrors Bullet's split-impulse
 * angular limiting). */
#define PH3D_MAX_ANGULAR_CORRECTION 0.05

/// @brief Nudge a body's orientation by the small rotation vector @p dtheta.
/// @param[in,out] body Borrowed body whose quaternion is integrated and renormalized.
/// @param dtheta Borrowed finite three-element axis-angle increment in radians.
static void ph3d_apply_position_rotation(rt_body3d *body, const double *dtheta) {
    if (!body || !ph3d_vec3_all_finite(dtheta))
        return;
    if (fabs(dtheta[0]) < 1e-15 && fabs(dtheta[1]) < 1e-15 && fabs(dtheta[2]) < 1e-15)
        return;
    quat_integrate(body->orientation, dtheta, 1.0);
    quat_normalize(body->orientation);
}

/// @brief Split positional correction (nonlinear Gauss-Seidel) using each
///   manifold's deepest point, weighted by the full contact effective mass —
///   inverse mass AND inverse inertia. Runs after the velocity solve so it
///   never injects energy into the velocity state.
/// @details The angular term is what lets a tilted box against a corner or a
///   leaning stack actually rotate out of penetration; the old translation-only
///   correction could never remove rotational overlap. Angular corrections are
///   capped per pass (PH3D_MAX_ANGULAR_CORRECTION) and every manifold point's
///   stored separation is advanced by the normal-displacement THAT point
///   experienced (linear + lever-arm terms) so extra position iterations
///   converge instead of compounding.
/// @param[in,out] c Contact manifold whose dynamic-body poses and point separations are corrected.
/// @param beta Clamped Baumgarte correction fraction applied to penetration beyond the slop.
static void world3d_solve_position_contact(rt_contact3d *c, double beta) {
    rt_body3d *a;
    rt_body3d *b;
    const double *n;
    double r_a[3];
    double r_b[3];
    double denom;
    double pen;
    double corr;
    double impulse[3];
    double next_a[3];
    double next_b[3];
    double dtheta_a[3] = {0.0, 0.0, 0.0};
    double dtheta_b[3] = {0.0, 0.0, 0.0};
    int32_t deepest = 0;
    if (!c)
        return;
    a = c->body_a;
    b = c->body_b;
    if (!a || !b || c->is_trigger || c->contact_count <= 0 ||
        (!ph3d_body_is_active(a) && !ph3d_body_is_active(b)))
        return;
    for (int32_t k = 1; k < c->contact_count; ++k) {
        if (c->separations[k] < c->separations[deepest])
            deepest = k;
    }
    pen = -c->separations[deepest] - PH3D_PENETRATION_SLOP;
    if (pen <= 0.0)
        return;
    n = c->normals[deepest];
    if (!ph3d_vec3_all_finite(n))
        return;
    vec3_sub(c->points[deepest], a->position, r_a);
    vec3_sub(c->points[deepest], b->position, r_b);
    denom = body3d_contact_impulse_denominator(a, r_a, n) +
            body3d_contact_impulse_denominator(b, r_b, n);
    if (!isfinite(denom) || denom <= 1e-12)
        return;
    /* Clamp the positional projection so a deep penetration cannot teleport
     * a body (which would otherwise inject huge separation and explode a
     * stack). Mirrors Box2D's b2_maxLinearCorrection. */
    pen = fmin(pen, PH3D_MAX_POSITION_CORRECTION);
    corr = beta * pen / denom;
    if (!isfinite(corr))
        return;
    impulse[0] = corr * n[0];
    impulse[1] = corr * n[1];
    impulse[2] = corr * n[2];
    for (int axis = 0; axis < 3; axis++) {
        next_a[axis] = a->position[axis] - impulse[axis] * a->inv_mass;
        next_b[axis] = b->position[axis] + impulse[axis] * b->inv_mass;
    }
    if (!ph3d_vec3_all_finite(next_a) || !ph3d_vec3_all_finite(next_b))
        return;
    /* Angular pseudo-impulse: dtheta = I^-1 (r x J), clamped per pass. The
     * torque is a world-space vector, so it must go through the world-space
     * inverse inertia (R * I_local^-1 * R^T) — the same transform the scalar
     * `denom` above used — not the raw body-local diagonal. */
    {
        double torque_a[3];
        double torque_b[3];
        double mag;
        vec3_cross(r_a, impulse, torque_a);
        vec3_cross(r_b, impulse, torque_b);
        body3d_world_inv_inertia_mul(a, torque_a, dtheta_a);
        body3d_world_inv_inertia_mul(b, torque_b, dtheta_b);
        for (int axis = 0; axis < 3; axis++)
            dtheta_a[axis] = -dtheta_a[axis];
        mag = vec3_len(dtheta_a);
        if (isfinite(mag) && mag > PH3D_MAX_ANGULAR_CORRECTION)
            vec3_scale_in_place(dtheta_a, PH3D_MAX_ANGULAR_CORRECTION / mag);
        mag = vec3_len(dtheta_b);
        if (isfinite(mag) && mag > PH3D_MAX_ANGULAR_CORRECTION)
            vec3_scale_in_place(dtheta_b, PH3D_MAX_ANGULAR_CORRECTION / mag);
        if (!ph3d_vec3_all_finite(dtheta_a))
            vec3_set(dtheta_a, 0.0, 0.0, 0.0);
        if (!ph3d_vec3_all_finite(dtheta_b))
            vec3_set(dtheta_b, 0.0, 0.0, 0.0);
    }
    /* Only dynamic bodies can move here (inv_mass/inv_inertia are zero for the
     * rest, so next == position and dtheta == 0). Skipping the writes for
     * non-dynamic bodies also removes a formal data race: a static body shared
     * by two islands would otherwise be written (with identical values) from
     * two solver threads at once. */
    if (a->motion_mode == PH3D_MODE_DYNAMIC) {
        vec3_copy(a->position, next_a);
        ph3d_apply_position_rotation(a, dtheta_a);
    }
    if (b->motion_mode == PH3D_MODE_DYNAMIC) {
        vec3_copy(b->position, next_b);
        ph3d_apply_position_rotation(b, dtheta_b);
    }
    /* Record the penetration removed this pass so the next position iteration
     * reads the reduced overlap instead of re-applying the detection-time
     * separation (nonlinear Gauss-Seidel; without this, raising
     * position_iterations multiplies the ejection and blows stacks apart).
     * Each point's separation grows by the normal displacement at THAT point:
     * the uniform linear part plus the lever-arm (dtheta x r) parts. */
    {
        double linear_applied = corr * (a->inv_mass + b->inv_mass);
        for (int32_t k = 0; k < c->contact_count; ++k) {
            double rk_a[3];
            double rk_b[3];
            double spin_a[3];
            double spin_b[3];
            double delta;
            vec3_sub(c->points[k], a->position, rk_a);
            vec3_sub(c->points[k], b->position, rk_b);
            vec3_cross(dtheta_b, rk_b, spin_b);
            vec3_cross(dtheta_a, rk_a, spin_a);
            delta = linear_applied + vec3_dot(spin_b, n) - vec3_dot(spin_a, n);
            if (isfinite(delta))
                c->separations[k] += delta;
        }
    }
    /* Bump only the body-local broadphase revisions here: this runs inside the
     * (possibly parallel) island pass, and bodies belong to exactly one island.
     * The world-level query_broadphase_dirty flag is set once, serially, by the
     * position-pass dispatcher (world3d_solve_position_solver_islands). */
    if (a->inv_mass > 0.0)
        a->broadphase_revision =
            a->broadphase_revision == UINT64_MAX ? 1u : a->broadphase_revision + 1u;
    if (b->inv_mass > 0.0)
        b->broadphase_revision =
            b->broadphase_revision == UINT64_MAX ? 1u : b->broadphase_revision + 1u;
}

/// @brief Warm-start every solvable contact, walking the batch island by island.
/// @param w Borrowed world containing current and previous contact manifolds.
/// @param batch Borrowed island layout selecting current contacts in deterministic order.
void world3d_warm_start_solver_islands(rt_world3d *w, const ph3d_solver_island_batch *batch) {
    contact_pair_hash_entry *prev_table;
    int32_t prev_capacity = 0;
    if (!w || !batch)
        return;
    /* Index the previous frame's contacts once so each current contact matches in
     * O(1) instead of scanning all previous contacts (was O(current x previous) per
     * substep). Built in the world's persistent pair-table scratch — no per-substep
     * malloc/free. NULL on empty/alloc-failure falls back to the linear scan. */
    prev_table = contact_pair_table_build_scratch(w,
                                                  PH3D_SCRATCH_PAIR_TABLE_A,
                                                  w->previous_contacts,
                                                  w->previous_contact_count,
                                                  &prev_capacity);
    for (int32_t island = 0; island < batch->island_count; ++island) {
        int32_t begin = batch->island_offsets[island];
        int32_t end = batch->island_offsets[island + 1];
        for (int32_t i = begin; i < end; ++i)
            world3d_warm_start_contact(
                w, &w->contacts[batch->contact_indices[i]], prev_table, prev_capacity);
    }
}

/// @brief Run one velocity-solve pass over every solvable contact, island by island
///   (parallel across islands when the scene is large enough to profit).
/// @param batch Borrowed island layout selecting mutually independent contact groups.
/// @param w Borrowed world whose contact impulses and body velocities are updated.
void world3d_solve_velocity_solver_islands(const ph3d_solver_island_batch *batch, rt_world3d *w) {
    if (!batch || !w)
        return;
    world3d_solve_islands_dispatch(w, batch, 0, 0.0);
}

/// @brief Run one position-correction pass over every solvable contact, island by
///   island (parallel across islands when profitable).
/// @param batch Borrowed island layout selecting mutually independent contact groups.
/// @param w Borrowed world whose dynamic-body poses and query-cache dirty flag are updated.
void world3d_solve_position_solver_islands(const ph3d_solver_island_batch *batch, rt_world3d *w) {
    double beta;
    if (!batch || !w)
        return;
    beta = clampd(ph3d_finite_or(w->contact_beta, PH3D_DEFAULT_CONTACT_BETA), 0.0, 1.0);
    world3d_solve_islands_dispatch(w, batch, 1, beta);
    /* Positions may have moved: mark the query broadphase lazily dirty exactly
     * once, serially (the per-contact touch inside the parallel pass bumps only
     * body-local revisions to stay race-free). */
    if (batch->solver_contact_count > 0)
        w->query_broadphase_dirty = 1;
}

/// @brief Publish the per-contact scalar normal impulse (summed over manifold
///   points) for collision-event payloads and wake bodies that took a load.
/// @param[in,out] w Borrowed world whose current contacts and impacted bodies are finalized.
void world3d_finalize_contacts(rt_world3d *w) {
    if (!w)
        return;
    for (int32_t i = 0; i < w->contact_count; ++i) {
        rt_contact3d *c = &w->contacts[i];
        double total = 0.0;
        if (c->is_trigger) {
            c->normal_impulse = 0.0;
            continue;
        }
        for (int32_t k = 0; k < c->contact_count; ++k)
            total += c->normal_impulse_acc[k];
        c->normal_impulse = total;
        /* Wake on genuine impact only (approach speed), never on the sustained
         * support impulse of a resting contact — otherwise a settled stack can
         * never sleep, and a perpetually-re-solved stack slowly drifts. */
        if (c->relative_speed > PH3D_CONTACT_WAKE_SPEED && c->body_a && c->body_b) {
            body3d_wake_if_dynamic(c->body_a);
            body3d_wake_if_dynamic(c->body_b);
        }
    }
}

/// @brief Post-solve sleep update: a dynamic body whose *resolved* velocity stays
///   below the sleep thresholds for PH3D_SLEEP_DELAY goes to sleep (and is then
///   frozen by the contact gate). Run after the contact solve so a body resting
///   under gravity — momentarily fast mid-integration, ~0 once supported —
///   actually settles. Wake propagation across contacts keeps a body awake while
///   any neighbour it touches is still moving.
/// @param[in,out] w Borrowed world whose eligible dynamic bodies are evaluated.
/// @param sub_dt Elapsed solver substep duration in seconds added to low-motion sleep timers.
void world3d_update_sleep(rt_world3d *w, double sub_dt) {
    if (!w)
        return;
    for (int32_t i = 0; i < w->body_count; i++) {
        rt_body3d *b = w->bodies[i];
        double linear_sq;
        double angular_sq;
        double linear_thresh = PH3D_SLEEP_LINEAR_THRESHOLD * PH3D_SLEEP_LINEAR_THRESHOLD;
        double angular_thresh = PH3D_SLEEP_ANGULAR_THRESHOLD * PH3D_SLEEP_ANGULAR_THRESHOLD;
        if (!b || b->motion_mode != PH3D_MODE_DYNAMIC || b->is_sleeping || !b->can_sleep)
            continue;
        linear_sq = vec3_len_sq(b->velocity);
        angular_sq = vec3_len_sq(b->angular_velocity);
        if (linear_sq <= linear_thresh && angular_sq <= angular_thresh) {
            b->sleep_time += sub_dt;
            if (b->sleep_time >= PH3D_SLEEP_DELAY) {
                b->is_sleeping = 1;
                vec3_set(b->velocity, 0.0, 0.0, 0.0);
                vec3_set(b->angular_velocity, 0.0, 0.0, 0.0);
            }
        } else {
            b->sleep_time = 0.0;
        }
    }
}

/// @brief Order two broad-phase entries along @p primary (0=X, 1=Y, 2=Z), then the
///   other two axes, then the body's stable @c owner_index.
/// @details Tie-breaking on @c owner_index — the body's slot index within the world,
///   which is identical in the VM and native backends and stable run-to-run — rather
///   than the body's heap pointer keeps sweep-and-prune order deterministic. Pointer
///   values vary with allocation order / ASLR and would otherwise desynchronise the
///   order-sensitive warm-started sequential-impulse solve between runs and backends.
/// @param a Borrowed first entry.
/// @param b Borrowed second entry.
/// @param primary Axis (0/1/2) used as the primary sort key.
/// @return -1, 0, or +1 for sort-order comparisons.
int ph3d_broadphase_compare_entries_axis(const ph3d_broadphase_entry *a,
                                         const ph3d_broadphase_entry *b,
                                         int primary) {
    const int a1 = (primary + 1) % 3;
    const int a2 = (primary + 2) % 3;
    if (a->min[primary] < b->min[primary])
        return -1;
    if (a->min[primary] > b->min[primary])
        return 1;
    if (a->min[a1] < b->min[a1])
        return -1;
    if (a->min[a1] > b->min[a1])
        return 1;
    if (a->min[a2] < b->min[a2])
        return -1;
    if (a->min[a2] > b->min[a2])
        return 1;
    {
        int32_t ia = a->body ? a->body->owner_index : -1;
        int32_t ib = b->body ? b->body->owner_index : -1;
        if (ia < ib)
            return -1;
        if (ia > ib)
            return 1;
    }
    return 0;
}

/// @brief Legacy comparator adapter for sweep-and-prune broad-phase entries by min-X.
/// @details After sorting, the inner collision loop can break early as soon as
///   entry[j].min[0] > entry[i].max[0], reducing the O(n²) pair count in practice.
///   Non-static for tests and internal code that still wants a C comparator adapter.
/// @param lhs Borrowed pointer to the first `ph3d_broadphase_entry`.
/// @param rhs Borrowed pointer to the second `ph3d_broadphase_entry`.
/// @return Negative, zero, or positive according to deterministic minimum-X ordering.
int ph3d_broadphase_compare_min_x(const void *lhs, const void *rhs) {
    return ph3d_broadphase_compare_entries_axis(
        (const ph3d_broadphase_entry *)lhs, (const ph3d_broadphase_entry *)rhs, 0);
}

/// @brief Sort a small or nearly sorted broadphase entry array with insertion sort.
/// @details Motion between physics steps is usually coherent, so insertion sort is faster than a
///   general-purpose indirect-comparator sort when the previous order is mostly preserved.
/// @param[in,out] entries Array to sort in place.
/// @param count Number of initialized entries.
/// @param axis Primary sort axis, with the remaining axes and owner index used as tie-breakers.
static void ph3d_broadphase_insertion_sort(ph3d_broadphase_entry *entries,
                                           int32_t count,
                                           int axis) {
    if (!entries || count <= 1)
        return;
    for (int32_t i = 1; i < count; i++) {
        ph3d_broadphase_entry key = entries[i];
        int32_t j = i - 1;
        while (j >= 0 && ph3d_broadphase_compare_entries_axis(&entries[j], &key, axis) > 0) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

/// @brief Count adjacent inversions up to @p limit to decide whether insertion sort is cheap.
/// @details The scan short-circuits once the data is clearly unordered; it is a small O(n) probe
///   that avoids running insertion sort on fully scrambled broadphase arrays.
/// @param entries Borrowed entry array to sample.
/// @param count Number of initialized entries.
/// @param axis Primary comparison axis.
/// @param limit Positive count at which the probe stops.
/// @return The number of inversions observed, capped at @p limit; zero for invalid or trivial
/// input.
static int32_t ph3d_broadphase_adjacent_inversions(const ph3d_broadphase_entry *entries,
                                                   int32_t count,
                                                   int axis,
                                                   int32_t limit) {
    int32_t inversions = 0;
    if (!entries || count <= 1 || limit <= 0)
        return 0;
    for (int32_t i = 1; i < count; i++) {
        if (ph3d_broadphase_compare_entries_axis(&entries[i - 1], &entries[i], axis) > 0 &&
            ++inversions >= limit)
            break;
    }
    return inversions;
}

/// @brief Merge two sorted broadphase runs from @p src into @p dst.
/// @details Used by the bottom-up stable merge sort fallback. Stable ordering preserves the
///   owner-index tie break and keeps solver warm-start behavior deterministic.
/// @param src Borrowed source array containing both sorted half-open runs.
/// @param[out] dst Destination array receiving their stable merge.
/// @param left Inclusive beginning of the first run.
/// @param mid Exclusive end of the first run and inclusive beginning of the second.
/// @param right Exclusive end of the second run.
/// @param axis Primary comparison axis.
static void ph3d_broadphase_merge_run(const ph3d_broadphase_entry *src,
                                      ph3d_broadphase_entry *dst,
                                      int32_t left,
                                      int32_t mid,
                                      int32_t right,
                                      int axis) {
    int32_t i = left;
    int32_t j = mid;
    int32_t out = left;
    while (i < mid && j < right) {
        if (ph3d_broadphase_compare_entries_axis(&src[i], &src[j], axis) <= 0)
            dst[out++] = src[i++];
        else
            dst[out++] = src[j++];
    }
    while (i < mid)
        dst[out++] = src[i++];
    while (j < right)
        dst[out++] = src[j++];
}

/// @brief Deterministically sort broadphase entries along @p axis without calling libc qsort.
/// @details Uses insertion sort for small/nearly-sorted arrays and a bottom-up stable merge sort
///   for larger unordered arrays. @p scratch must hold @p count entries for the merge path; if it
///   is unavailable the function falls back to insertion sort to preserve correctness.
/// @param[in,out] entries Array of entries to sort in place.
/// @param scratch Optional caller-owned array with capacity for @p count entries.
/// @param count Number of initialized entries.
/// @param axis Primary sort axis, expected to be 0, 1, or 2.
void ph3d_broadphase_sort_entries(ph3d_broadphase_entry *entries,
                                  ph3d_broadphase_entry *scratch,
                                  int32_t count,
                                  int axis) {
    const int32_t insertion_limit = 64;
    const int32_t inversion_limit = 8;
    if (!entries || count <= 1)
        return;
    if (count <= insertion_limit ||
        ph3d_broadphase_adjacent_inversions(entries, count, axis, inversion_limit) <
            inversion_limit ||
        !scratch) {
        ph3d_broadphase_insertion_sort(entries, count, axis);
        return;
    }

    ph3d_broadphase_entry *src = entries;
    ph3d_broadphase_entry *dst = scratch;
    for (int32_t width = 1; width < count; width *= 2) {
        for (int32_t left = 0; left < count; left += width * 2) {
            int32_t mid = left + width;
            int32_t right = left + width * 2;
            if (mid > count)
                mid = count;
            if (right > count)
                right = count;
            ph3d_broadphase_merge_run(src, dst, left, mid, right, axis);
        }
        ph3d_broadphase_entry *tmp = src;
        src = dst;
        dst = tmp;
    }
    if (src != entries)
        memcpy(entries, src, (size_t)count * sizeof(*entries));
}

/// @brief Return 1 if two 3D AABBs overlap, 0 if separated on any axis.
/// @details Separating-axis test on all three axes simultaneously.  Used in the
///   broad phase after the X-axis early-out to confirm overlap on Y and Z.
/// @param a_min Borrowed three-element minimum corner of the first AABB.
/// @param a_max Borrowed three-element maximum corner of the first AABB.
/// @param b_min Borrowed three-element minimum corner of the second AABB.
/// @param b_max Borrowed three-element maximum corner of the second AABB.
/// @return `1` when the inclusive bounds overlap or touch on all axes; otherwise `0`.
static int ph3d_bounds_overlap(const double *a_min,
                               const double *a_max,
                               const double *b_min,
                               const double *b_max) {
    return a_min[0] <= b_max[0] && a_max[0] >= b_min[0] && a_min[1] <= b_max[1] &&
           a_max[1] >= b_min[1] && a_min[2] <= b_max[2] && a_max[2] >= b_min[2];
}

/// @brief Run narrow-phase collision detection for one body pair (no resolution).
/// @details Skips static-static pairs or pairs that fail the bidirectional
///   layer/mask filter.  On a detected overlap, appends a new rt_contact3d to the
///   world's contact array (reallocating if needed) with its full manifold and a
///   zeroed warm-start state; the warm-started sequential-impulse solver resolves
///   the assembled contact list afterwards. Records the pre-solve approach speed
///   and updates is_grounded on whichever body has a contact normal pointing more
///   than ~45° upward (|normal.y| > 0.7).
/// @param w  World containing the bodies.
/// @param a  First body of the candidate pair.
/// @param b  Second body of the candidate pair.
/// @return   1 on success (including skipped pairs), 0 if the contact array
///           could not be reallocated.
static int world3d_process_collision_pair(rt_world3d *w, rt_body3d *a, rt_body3d *b) {
    double normal[3], depth, point[3];
    double relative_speed = 0.0;
    void *leaf_a = NULL;
    void *leaf_b = NULL;
    rt_collider_pose leaf_a_pose;
    rt_collider_pose leaf_b_pose;
    rt_contact3d manifold_probe;

    if (!w || !a || !b || a == b)
        return 1;
    /* Canonicalize pair order by stable body index so a persistent pair keeps
     * the same A/B roles (and manifold normal sign) when the broadphase sweep
     * axis flips between frames. Index-based warm-start matching compares
     * body_a/body_b directly; without a canonical order an axis flip swaps the
     * roles, the normal-agreement guard rejects the seed, and settled stacks
     * transiently soften. Deterministic: indices come from creation order. */
    if (world3d_body_index_of(w, b) < world3d_body_index_of(w, a)) {
        rt_body3d *swap_body = a;
        a = b;
        b = swap_body;
    }
    if (a->motion_mode == PH3D_MODE_STATIC && b->motion_mode == PH3D_MODE_STATIC)
        return 1;
    if (!(a->collision_layer & b->collision_mask))
        return 1;
    if (!(b->collision_layer & a->collision_mask))
        return 1;
    memset(&manifold_probe, 0, sizeof(manifold_probe));
    if (!test_collision_manifold(a,
                                 b,
                                 normal,
                                 &depth,
                                 point,
                                 &leaf_a,
                                 &leaf_b,
                                 &leaf_a_pose,
                                 &leaf_b_pose,
                                 &manifold_probe))
        return 1;

    int32_t next_contact_count;
    if (!world3d_checked_increment(w->contact_count, &next_contact_count) ||
        !world3d_reserve_contacts(w, next_contact_count)) {
        world3d_step_fail(w, "Physics3D.World.Step: contact allocation failed");
        return 0;
    }

    rt_contact3d *c = &w->contacts[w->contact_count++];
    {
        rt_contact3d zero = {0};
        *c = zero;
    }
    c->body_a = a;
    c->body_b = b;
    c->collider_a = leaf_a ? leaf_a : a->collider;
    c->collider_b = leaf_b ? leaf_b : b->collider;
    c->point[0] = point[0];
    c->point[1] = point[1];
    c->point[2] = point[2];
    c->normal[0] = normal[0];
    c->normal[1] = normal[1];
    c->normal[2] = normal[2];
    c->separation = -depth;
    contact3d_init_single_point(c, point, normal, c->separation);
    /* Compound leaf contacts were accumulated during the narrow phase itself
     * (test_collision_manifold); merging them here replaces the old second full
     * compound narrow-phase pass. The dedup guard absorbs the primary point. */
    for (int32_t k = 0; k < manifold_probe.contact_count; ++k)
        contact3d_try_add_manifold_point(
            c, manifold_probe.points[k], manifold_probe.normals[k], manifold_probe.separations[k]);
    if (c->contact_count <= 1)
        contact3d_expand_compound_manifold(c, a, b);
    contact3d_expand_aabb_manifold(c, a, b);
    if (c->contact_count <= 1)
        contact3d_expand_obb_manifold(c, leaf_a, &leaf_a_pose, leaf_b, &leaf_b_pose);
    if (c->contact_count <= 1)
        contact3d_expand_capsule_manifold(c, a, b);
    /* Mesh/heightfield narrow phases report one deepest point; grow it into a
     * support patch so crates and hulls resting on terrain or mesh floors get a
     * stable multi-point manifold instead of balancing (and rocking) on a
     * single contact. */
    if (c->contact_count <= 1 && leaf_a && leaf_b)
        contact3d_expand_surface_support_manifold(c, leaf_a, &leaf_a_pose, leaf_b, &leaf_b_pose);
    c->is_trigger = (a->is_trigger || b->is_trigger) ? 1 : 0;
    /* Detection only: the warm-started sequential-impulse solver runs after the
     * whole manifold is built. Record the
     * pre-solve approach speed for collision-event payloads and reset the
     * per-point accumulated impulses; warm starting seeds them next step. */
    relative_speed = fabs((b->velocity[0] - a->velocity[0]) * normal[0] +
                          (b->velocity[1] - a->velocity[1]) * normal[1] +
                          (b->velocity[2] - a->velocity[2]) * normal[2]);
    c->relative_speed = relative_speed;
    c->normal_impulse = 0.0;
    memset(c->normal_impulse_acc, 0, sizeof(c->normal_impulse_acc));
    memset(c->tangent_impulse_acc, 0, sizeof(c->tangent_impulse_acc));
    memset(c->restitution_bias, 0, sizeof(c->restitution_bias));

    /* Grounded means "supported": require the underneath body to be a static or
     * kinematic support OR the pair to be approaching/resting along the normal.
     * Without the gate a mid-air glancing corner hit between two dynamic bodies
     * (normal.y just over the slope threshold while they separate) granted a
     * one-frame false is_grounded, letting jump logic re-fire in the air. */
    {
        double rel_vn = (b->velocity[0] - a->velocity[0]) * normal[0] +
                        (b->velocity[1] - a->velocity[1]) * normal[1] +
                        (b->velocity[2] - a->velocity[2]) * normal[2];
        if (normal[1] > 0.7 &&
            (a->motion_mode != PH3D_MODE_DYNAMIC || rel_vn <= PH3D_CONTACT_WAKE_SPEED)) {
            b->is_grounded = 1;
            b->ground_normal[0] = normal[0];
            b->ground_normal[1] = normal[1];
            b->ground_normal[2] = normal[2];
        } else if (normal[1] < -0.7 &&
                   (b->motion_mode != PH3D_MODE_DYNAMIC || rel_vn <= PH3D_CONTACT_WAKE_SPEED)) {
            a->is_grounded = 1;
            a->ground_normal[0] = -normal[0];
            a->ground_normal[1] = -normal[1];
            a->ground_normal[2] = -normal[2];
        }
    }
    return 1;
}

/// @brief Cheap broad-phase rejection test for a body pair before narrow-phase: rejects
///   self-pairs, static-vs-static pairs, and pairs failing the bidirectional layer/mask
///   filter.
/// @param a Borrowed first candidate body.
/// @param b Borrowed second candidate body.
/// @return `1` only when the pair could plausibly collide; otherwise `0`.
static int world3d_pair_can_collide_cheap(const rt_body3d *a, const rt_body3d *b) {
    if (!a || !b || a == b)
        return 0;
    if (a->motion_mode == PH3D_MODE_STATIC && b->motion_mode == PH3D_MODE_STATIC)
        return 0;
    if (!(a->collision_layer & b->collision_mask))
        return 0;
    if (!(b->collision_layer & a->collision_mask))
        return 0;
    return 1;
}

/// @brief Count bodies that actually contribute a broadphase AABB this step.
/// @details Capacity reservations use this count instead of the raw body array size so worlds with
///   many placeholder bodies do not over-reserve broadphase scratch.
/// @param w Borrowed world whose collision-capable bodies are counted.
/// @return The number of bodies with collision geometry, or zero for a null world.
static int32_t world3d_count_broadphase_bodies(const rt_world3d *w) {
    int32_t count = 0;
    if (!w)
        return 0;
    for (int32_t i = 0; i < w->body_count; i++) {
        if (body3d_has_collision_geometry(w->bodies[i]))
            count++;
    }
    return count;
}

/// @brief Fill @p entries with current body AABBs for every collision-capable body in @p w.
/// @details The caller guarantees enough capacity. Separating this from allocation lets the step
///   path use persistent world scratch or a bounded stack fallback without duplicating AABB logic.
/// @param w Borrowed world supplying collision-capable bodies.
/// @param[out] entries Caller-owned array with capacity for every contributing body.
/// @return The number of initialized entries, or zero for invalid arguments or an empty world.
static int32_t world3d_fill_broadphase_entries(rt_world3d *w, ph3d_broadphase_entry *entries) {
    int32_t entry_count = 0;
    if (!w || !entries)
        return 0;
    for (int32_t i = 0; i < w->body_count; i++) {
        rt_body3d *body = w->bodies[i];
        if (!body3d_has_collision_geometry(body))
            continue;
        entries[entry_count].body = body;
        body_aabb(body, entries[entry_count].min, entries[entry_count].max);
        /* The step broadphase never reads revisions; stamp so the merge sort
         * only ever copies initialized bytes. */
        entries[entry_count].body_revision = 0;
        entry_count++;
    }
    return entry_count;
}

typedef struct {
    double min[3];
    double max[3];
    int32_t left;
    int32_t right;
    int32_t entry;
} ph3d_broadphase_bvh_node;

/// @brief Choose the widest centre-spread axis for one broadphase entry range.
/// @param entries Borrowed entry range.
/// @param count Positive entry count.
/// @return Axis index in `[0, 2]`, with deterministic lower-axis tie breaking.
static int ph3d_broadphase_range_axis(const ph3d_broadphase_entry *entries, int32_t count) {
    double lo[3];
    double hi[3];
    int axis = 0;
    for (int k = 0; k < 3; ++k)
        lo[k] = hi[k] = 0.5 * entries[0].min[k] + 0.5 * entries[0].max[k];
    for (int32_t i = 1; i < count; ++i) {
        for (int k = 0; k < 3; ++k) {
            double center = 0.5 * entries[i].min[k] + 0.5 * entries[i].max[k];
            if (center < lo[k])
                lo[k] = center;
            if (center > hi[k])
                hi[k] = center;
        }
    }
    if (hi[1] - lo[1] > hi[axis] - lo[axis])
        axis = 1;
    if (hi[2] - lo[2] > hi[axis] - lo[axis])
        axis = 2;
    return axis;
}

/// @brief Swap two broadphase entries in place.
static void ph3d_broadphase_swap_entries(ph3d_broadphase_entry *a, ph3d_broadphase_entry *b) {
    ph3d_broadphase_entry temporary = *a;
    *a = *b;
    *b = temporary;
}

/// @brief Partition a broadphase range until the requested median occupies its sorted position.
/// @details A deterministic median-of-three pivot plus three-way partition avoids recursively
/// sorting both halves merely to discover one split point. Equal center coordinates are ordered by
/// the existing stable body key, so construction stays repeatable across platforms. Each BVH level
/// now performs linear partition work instead of a full O(n log n) subrange sort.
static void ph3d_broadphase_select_nth(
    ph3d_broadphase_entry *entries, int32_t begin, int32_t end, int32_t nth, int axis) {
    while (end - begin > 1) {
        int32_t first = begin;
        int32_t middle = begin + (end - begin) / 2;
        int32_t last = end - 1;
        if (ph3d_broadphase_compare_entries_axis(&entries[first], &entries[middle], axis) > 0)
            ph3d_broadphase_swap_entries(&entries[first], &entries[middle]);
        if (ph3d_broadphase_compare_entries_axis(&entries[middle], &entries[last], axis) > 0)
            ph3d_broadphase_swap_entries(&entries[middle], &entries[last]);
        if (ph3d_broadphase_compare_entries_axis(&entries[first], &entries[middle], axis) > 0)
            ph3d_broadphase_swap_entries(&entries[first], &entries[middle]);
        ph3d_broadphase_entry pivot = entries[middle];
        int32_t lower = begin;
        int32_t cursor = begin;
        int32_t upper = end;
        while (cursor < upper) {
            int comparison = ph3d_broadphase_compare_entries_axis(&entries[cursor], &pivot, axis);
            if (comparison < 0) {
                ph3d_broadphase_swap_entries(&entries[lower++], &entries[cursor++]);
            } else if (comparison > 0) {
                ph3d_broadphase_swap_entries(&entries[cursor], &entries[--upper]);
            } else {
                cursor++;
            }
        }
        if (nth < lower)
            end = lower;
        else if (nth >= upper)
            begin = upper;
        else
            return;
    }
}

/// @brief Recursively build a balanced spatial AABB hierarchy over an entry range.
/// @details Each internal range is partitioned at its median on the widest centre axis. This
/// prevents one-axis dense layouts from degrading candidate traversal without paying for a full
/// sort at every recursive node.
/// @param entries Mutable entry range; reordered deterministically during construction.
/// @param begin First entry in the range.
/// @param end One-past-last entry in the range.
/// @param nodes Caller-owned node array with capacity `2 * entry_count - 1`.
/// @param node_cursor Next unused node slot, advanced for every constructed node.
/// @return Root node index for the range.
static int32_t ph3d_broadphase_bvh_build(ph3d_broadphase_entry *entries,
                                         int32_t begin,
                                         int32_t end,
                                         ph3d_broadphase_bvh_node *nodes,
                                         int32_t *node_cursor) {
    int32_t node_index = (*node_cursor)++;
    ph3d_broadphase_bvh_node *node = &nodes[node_index];
    int32_t count = end - begin;
    if (count == 1) {
        node->left = -1;
        node->right = -1;
        node->entry = begin;
        memcpy(node->min, entries[begin].min, sizeof(node->min));
        memcpy(node->max, entries[begin].max, sizeof(node->max));
        return node_index;
    }
    {
        int axis = ph3d_broadphase_range_axis(entries + begin, count);
        int32_t middle = begin + count / 2;
        ph3d_broadphase_select_nth(entries, begin, end, middle, axis);
        node->left = ph3d_broadphase_bvh_build(entries, begin, middle, nodes, node_cursor);
        node->right = ph3d_broadphase_bvh_build(entries, middle, end, nodes, node_cursor);
    }
    node->entry = -1;
    for (int k = 0; k < 3; ++k) {
        const ph3d_broadphase_bvh_node *left = &nodes[node->left];
        const ph3d_broadphase_bvh_node *right = &nodes[node->right];
        node->min[k] = left->min[k] < right->min[k] ? left->min[k] : right->min[k];
        node->max[k] = left->max[k] > right->max[k] ? left->max[k] : right->max[k];
    }
    return node_index;
}

/// @brief Return an AABB node's extent sum for deterministic traversal splitting.
static double ph3d_broadphase_bvh_extent_sum(const ph3d_broadphase_bvh_node *node) {
    return (node->max[0] - node->min[0]) + (node->max[1] - node->min[1]) +
           (node->max[2] - node->min[2]);
}

/// @brief Traverse one unordered node pair and process every overlapping leaf pair exactly once.
/// @param w World receiving contacts and broadphase diagnostics.
/// @param entries Borrowed leaf entry array.
/// @param nodes Borrowed hierarchy node array.
/// @param node_a First hierarchy node.
/// @param node_b Second hierarchy node; may equal @p node_a for self traversal.
/// @return One on success, zero when contact allocation/processing fails.
static int world3d_process_broadphase_bvh_pair(rt_world3d *w,
                                               ph3d_broadphase_entry *entries,
                                               const ph3d_broadphase_bvh_node *nodes,
                                               int32_t node_a,
                                               int32_t node_b) {
    const ph3d_broadphase_bvh_node *a = &nodes[node_a];
    const ph3d_broadphase_bvh_node *b = &nodes[node_b];
    if (!ph3d_bounds_overlap(a->min, a->max, b->min, b->max))
        return 1;
    if (node_a == node_b) {
        if (a->entry >= 0)
            return 1;
        return world3d_process_broadphase_bvh_pair(w, entries, nodes, a->left, a->left) &&
               world3d_process_broadphase_bvh_pair(w, entries, nodes, a->left, a->right) &&
               world3d_process_broadphase_bvh_pair(w, entries, nodes, a->right, a->right);
    }
    if (a->entry >= 0 && b->entry >= 0) {
        ph3d_broadphase_entry *entry_a = &entries[a->entry];
        ph3d_broadphase_entry *entry_b = &entries[b->entry];
        if (w->last_broadphase_leaf_pair_tests < INT64_MAX)
            w->last_broadphase_leaf_pair_tests++;
        if (!world3d_pair_can_collide_cheap(entry_a->body, entry_b->body))
            return 1;
        return world3d_process_collision_pair(w, entry_a->body, entry_b->body);
    }
    if (b->entry >= 0 ||
        (a->entry < 0 && ph3d_broadphase_bvh_extent_sum(a) >= ph3d_broadphase_bvh_extent_sum(b))) {
        return world3d_process_broadphase_bvh_pair(w, entries, nodes, a->left, node_b) &&
               world3d_process_broadphase_bvh_pair(w, entries, nodes, a->right, node_b);
    }
    return world3d_process_broadphase_bvh_pair(w, entries, nodes, node_a, b->left) &&
           world3d_process_broadphase_bvh_pair(w, entries, nodes, node_a, b->right);
}

/// @brief Run the full broad-phase + narrow-phase collision pass for one world step.
/// @details Clears the contact list from the previous step, then:
///   1. Attempts to allocate/reuse broadphase entries and BVH scratch.
///   2. If allocation fails for a small world, uses bounded stack scratch; large worlds fail
///      cleanly instead of silently entering an O(n²) collision crawl.
///   3. On success: computes each body's AABB, recursively median-splits on the widest centre
///      axis, and traverses overlapping hierarchy nodes to generate spatial leaf candidates.
///   Each surviving pair is processed by world3d_process_collision_pair, which
///   records the contact and its manifold. Resolution happens afterwards in the
///   warm-started sequential-impulse solve, not here.
/// @param w  World to process.
/// @return   1 on success, 0 if any contact could not be allocated.
int world3d_detect_contacts(rt_world3d *w) {
    if (!w)
        return 0;
    w->contact_count = 0;
    w->last_broadphase_leaf_pair_tests = 0;
    if (w->body_count <= 1)
        return 1;

    ph3d_broadphase_entry stack_entries[PH3D_BROADPHASE_STACK_FALLBACK];
    ph3d_broadphase_bvh_node stack_nodes[PH3D_BROADPHASE_STACK_FALLBACK * 2 - 1];
    int32_t geometry_count = world3d_count_broadphase_bodies(w);
    ph3d_broadphase_entry *entries = NULL;
    ph3d_broadphase_bvh_node *nodes = NULL;
    if (geometry_count <= 1)
        return 1;
    if (!world3d_reserve_broadphase_capacity(w, geometry_count)) {
        world3d_note_broadphase_fallback(w);
        if (geometry_count > PH3D_BROADPHASE_STACK_FALLBACK) {
            world3d_step_fail(w, "Physics3D.World.Step: broadphase allocation failed");
            return 0;
        }
        entries = stack_entries;
        nodes = stack_nodes;
    } else {
        entries = w->broadphase_entries;
        nodes = (ph3d_broadphase_bvh_node *)world3d_step_scratch_acquire(
            w, PH3D_SCRATCH_BROADPHASE_BVH, ((size_t)geometry_count * 2u - 1u) * sizeof(*nodes), 0);
        if (!nodes) {
            world3d_step_fail(w, "Physics3D.World.Step: broadphase hierarchy allocation failed");
            return 0;
        }
    }

    int32_t entry_count = world3d_fill_broadphase_entries(w, entries);
    {
        int32_t node_cursor = 0;
        int32_t root = ph3d_broadphase_bvh_build(entries, 0, entry_count, nodes, &node_cursor);
        if (node_cursor != entry_count * 2 - 1 ||
            !world3d_process_broadphase_bvh_pair(w, entries, nodes, root, root))
            return 0;
    }
    return 1;
}

#else
typedef int rt_physics3d_solver_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
