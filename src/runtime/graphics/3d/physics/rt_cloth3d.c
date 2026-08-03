//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_cloth3d.c
// Purpose: Verlet cloth simulator (Jakobsen-style constraint relaxation) with
//   chain and patch topologies, sphere/capsule pushout, pinning, wind, and
//   bone-chain / mesh output bindings. See rt_cloth3d.h.
// Key invariants:
//   - Fixed substep accumulator (default 1/120, max 8 substeps/step with the
//     remainder carried) makes replay deterministic for identical dt series.
//   - No wall-clock, no RNG; iteration order is fixed by construction order.
// Ownership/Lifetime:
//   - GC handle; retains bound mesh/animator; finalizer frees sim arrays.
// Links: ADR 0096, rt_cloth3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_cloth3d.c
 * @brief Implements deterministic Verlet cloth chains and rectangular patches.
 *
 * Cloth3D integrates fixed-size point sets with Jakobsen-style distance
 * relaxation, pin constraints, wind drag, and sphere or capsule pushout. A
 * fixed-step accumulator makes updates reproducible for identical time-step
 * sequences, while optional mesh and skeleton bindings publish the simulated
 * shape to rendering and animation consumers.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_cloth3d.h"

#include "rt_animcontroller3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_skeleton3d.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CLOTH3D_MAX_POINTS 4096
#define CLOTH3D_MAX_SEGMENTS 256
#define CLOTH3D_MAX_PATCH_DIM 64
#define CLOTH3D_MAX_COLLIDERS 16
#define CLOTH3D_MAX_CHAIN_BONES 32
#define CLOTH3D_MAX_SUBSTEPS 8
#define CLOTH3D_GRAVITY 9.81
#define CLOTH3D_MAX_GRAVITY_SCALE 1000000.0
#define CLOTH3D_MAX_WIND_RESPONSE 120.0
#define CLOTH3D_MAX_WIND_STRENGTH 1000000.0

/// @brief Distance constraint joining two Verlet points.
typedef struct cloth3d_constraint {
    ///< Index of the first constrained point.
    int32_t a;
    ///< Index of the second constrained point.
    int32_t b;
    ///< Required separation between the points.
    double rest;
} cloth3d_constraint;

/// @brief Static collision primitive used for cloth-point pushout.
typedef struct cloth3d_collider {
    ///< Nonzero for a capsule; zero for a sphere centered at @ref a.
    int8_t is_capsule;
    ///< Sphere center or capsule segment start.
    double a[3];
    ///< Capsule segment end; unused by spheres.
    double b[3];
    ///< Positive collision radius.
    double radius;
} cloth3d_collider;

/// @brief Cloth3D payload: verlet points, constraints, colliders, bindings.
typedef struct rt_cloth3d {
    double *pos;  /* 3 x point_count current positions */
    double *prev; /* 3 x point_count previous positions */
    uint8_t *pinned;
    double *pin_pos; /* 3 x point_count pin targets (valid where pinned) */
    int32_t point_count;
    cloth3d_constraint *constraints;
    int32_t constraint_count;
    int32_t width;  /* patch columns; chains: 1 */
    int32_t height; /* patch rows; chains: point count */
    double damping;
    double gravity_scale;
    double wind_response;
    double wind[3]; /* wind velocity vector (dir * strength) */
    int32_t iterations;
    double substep_dt;
    double accumulator;
    cloth3d_collider colliders[CLOTH3D_MAX_COLLIDERS];
    int32_t collider_count;
    /* Mesh binding (patch): vertices rewritten in place each step. */
    void *mesh;
    /* Bone-chain binding (chain): anchor + aim write-back via pose override. */
    void *animator;
    int32_t chain_bones[CLOTH3D_MAX_CHAIN_BONES];
    int32_t chain_bone_count;
    int8_t *override_mask;   /* skeleton bone count entries */
    float *override_globals; /* skeleton bone count x 16 */
    int32_t skeleton_bone_count;
    double total_rest; /* summed rest length (teleport threshold basis) */
    /* Stable allocation/reference identities and immutable topology metadata.
     * The fields above remain convenient hot-path mirrors; checked entry points
     * restore them from these authorities before any traversal or finalization. */
    double *owned_pos;
    double *owned_prev;
    uint8_t *owned_pinned;
    double *owned_pin_pos;
    cloth3d_constraint *owned_constraints;
    int32_t allocated_point_count;
    int32_t allocated_constraint_count;
    int32_t topology_width;
    int32_t topology_height;
    int32_t initialized_collider_count;
    void *owned_mesh;
    void *owned_animator;
    int32_t initialized_chain_bone_count;
    int8_t *owned_override_mask;
    float *owned_override_globals;
    int32_t allocated_skeleton_bone_count;
    int8_t anchor_initialized;
} rt_cloth3d;

/// @brief Clamp one cloth-space coordinate to the common Game3D finite range.
/// @param value Candidate coordinate.
/// @param fallback Finite replacement for NaN or infinity.
/// @return Finite coordinate in `[-RT_GAME3D_COORD_ABS_MAX, RT_GAME3D_COORD_ABS_MAX]`.
static double cloth3d_coord_or(double value, double fallback) {
    return game3d_clamp_coord_or(value, fallback);
}

/// @brief Overflow-resistant Euclidean length of a three-component vector.
/// @param value Three readable doubles.
/// @return Finite nonnegative length, or infinity for non-finite input.
static double cloth3d_length3(const double value[3]) {
    if (!value || !isfinite(value[0]) || !isfinite(value[1]) || !isfinite(value[2]))
        return INFINITY;
    return hypot(hypot(value[0], value[1]), value[2]);
}

/// @brief Return the sum of valid distance-constraint rest lengths.
/// @param cloth Cloth whose authoritative constraint table is inspected.
/// @return Positive finite sum capped to the common coordinate range.
static double cloth3d_sum_rest_lengths(const rt_cloth3d *cloth) {
    double sum = 0.0;
    int32_t count = cloth ? cloth->allocated_constraint_count : 0;
    if (!cloth || !cloth->owned_constraints || count <= 0)
        return 0.0;
    for (int32_t i = 0; i < count; ++i) {
        double rest = cloth->owned_constraints[i].rest;
        if (!isfinite(rest) || rest <= 0.0)
            continue;
        if (rest >= RT_GAME3D_COORD_ABS_MAX - sum)
            return RT_GAME3D_COORD_ABS_MAX;
        sum += rest;
    }
    return sum;
}

/// @brief Restore mutable cloth mirrors from stable ownership/topology authority.
/// @param cloth Cloth payload to repair; NULL is ignored.
static void cloth3d_repair_storage(rt_cloth3d *cloth) {
    if (!cloth)
        return;
    cloth->pos = cloth->owned_pos;
    cloth->prev = cloth->owned_prev;
    cloth->pinned = cloth->owned_pinned;
    cloth->pin_pos = cloth->owned_pin_pos;
    cloth->constraints = cloth->owned_constraints;
    cloth->point_count = cloth->allocated_point_count;
    cloth->constraint_count = cloth->allocated_constraint_count;
    cloth->width = cloth->topology_width;
    cloth->height = cloth->topology_height;
    cloth->mesh = cloth->owned_mesh;
    cloth->animator = cloth->owned_animator;
    cloth->override_mask = cloth->owned_override_mask;
    cloth->override_globals = cloth->owned_override_globals;
    cloth->skeleton_bone_count = cloth->allocated_skeleton_bone_count;
    if (cloth->initialized_collider_count < 0)
        cloth->initialized_collider_count = 0;
    if (cloth->initialized_collider_count > CLOTH3D_MAX_COLLIDERS)
        cloth->initialized_collider_count = CLOTH3D_MAX_COLLIDERS;
    cloth->collider_count = cloth->initialized_collider_count;
    if (cloth->initialized_chain_bone_count < 0)
        cloth->initialized_chain_bone_count = 0;
    if (cloth->initialized_chain_bone_count > CLOTH3D_MAX_CHAIN_BONES)
        cloth->initialized_chain_bone_count = CLOTH3D_MAX_CHAIN_BONES;
    cloth->chain_bone_count = cloth->initialized_chain_bone_count;
    cloth->anchor_initialized = cloth->anchor_initialized ? 1 : 0;
    if (!isfinite(cloth->damping))
        cloth->damping = 0.02;
    if (cloth->damping < 0.0)
        cloth->damping = 0.0;
    if (cloth->damping > 1.0)
        cloth->damping = 1.0;
    if (cloth->iterations < 1)
        cloth->iterations = 1;
    if (cloth->iterations > 32)
        cloth->iterations = 32;
    if (!isfinite(cloth->gravity_scale))
        cloth->gravity_scale = 1.0;
    if (cloth->gravity_scale > CLOTH3D_MAX_GRAVITY_SCALE)
        cloth->gravity_scale = CLOTH3D_MAX_GRAVITY_SCALE;
    if (cloth->gravity_scale < -CLOTH3D_MAX_GRAVITY_SCALE)
        cloth->gravity_scale = -CLOTH3D_MAX_GRAVITY_SCALE;
    if (!isfinite(cloth->wind_response) || cloth->wind_response < 0.0)
        cloth->wind_response = 1.0;
    if (cloth->wind_response > CLOTH3D_MAX_WIND_RESPONSE)
        cloth->wind_response = CLOTH3D_MAX_WIND_RESPONSE;
    for (int lane = 0; lane < 3; ++lane)
        cloth->wind[lane] = cloth3d_coord_or(cloth->wind[lane], 0.0);
    if (!isfinite(cloth->substep_dt) || cloth->substep_dt <= 0.0 || cloth->substep_dt > 1.0)
        cloth->substep_dt = 1.0 / 120.0;
    if (!isfinite(cloth->accumulator) || cloth->accumulator < 0.0)
        cloth->accumulator = 0.0;
    if (!isfinite(cloth->total_rest) || cloth->total_rest <= 0.0)
        cloth->total_rest = cloth3d_sum_rest_lengths(cloth);
    for (int32_t i = 0; i < cloth->collider_count; ++i) {
        cloth3d_collider *collider = &cloth->colliders[i];
        collider->is_capsule = collider->is_capsule ? 1 : 0;
        for (int lane = 0; lane < 3; ++lane) {
            collider->a[lane] = cloth3d_coord_or(collider->a[lane], 0.0);
            collider->b[lane] = cloth3d_coord_or(collider->b[lane], collider->a[lane]);
        }
        if (!isfinite(collider->radius) || collider->radius <= 0.0)
            collider->radius = 1.0;
        if (collider->radius > RT_GAME3D_COORD_ABS_MAX)
            collider->radius = RT_GAME3D_COORD_ABS_MAX;
    }
}

/// @brief Retain a typed binding in stable ownership and republish its mirror.
/// @param owner Stable retained slot.
/// @param mirror Mutable hot-path mirror.
/// @param value Optional new object.
/// @param class_id Required Graphics3D class.
static void cloth3d_assign_owned_ref(void **owner, void **mirror, void *value, int64_t class_id) {
    if (!owner || !mirror)
        return;
    if (*owner != value)
        game3d_assign_typed_ref(owner, value, class_id);
    *mirror = *owner;
}

/// @brief Validate a runtime object as Cloth3D and trap with caller-specific context on failure.
/// @param obj Candidate runtime handle.
/// @param method Diagnostic text passed to the trap handler when validation fails.
/// @return Valid Cloth3D payload, or NULL after reporting an invalid handle.
static rt_cloth3d *cloth3d_checked(void *obj, const char *method) {
    rt_cloth3d *cloth = (rt_cloth3d *)rt_g3d_checked_or_null(obj, RT_G3D_CLOTH3D_CLASS_ID);
    if (!cloth)
        rt_trap(method);
    cloth3d_repair_storage(cloth);
    return cloth;
}

/// @brief GC finalizer: free simulation arrays and release bindings.
/// @param obj Cloth3D payload being finalized; a null pointer is ignored.
static void cloth3d_finalize(void *obj) {
    rt_cloth3d *cloth = (rt_cloth3d *)obj;
    if (!cloth)
        return;
    free(cloth->owned_pos);
    free(cloth->owned_prev);
    free(cloth->owned_pinned);
    free(cloth->owned_pin_pos);
    free(cloth->owned_constraints);
    free(cloth->owned_override_mask);
    free(cloth->owned_override_globals);
    cloth->owned_pos = NULL;
    cloth->owned_prev = NULL;
    cloth->owned_pinned = NULL;
    cloth->owned_pin_pos = NULL;
    cloth->owned_constraints = NULL;
    cloth->owned_override_mask = NULL;
    cloth->owned_override_globals = NULL;
    cloth->pos = NULL;
    cloth->prev = NULL;
    cloth->pinned = NULL;
    cloth->pin_pos = NULL;
    cloth->constraints = NULL;
    cloth->override_mask = NULL;
    cloth->override_globals = NULL;
    game3d_release_typed_ref(&cloth->owned_mesh, RT_G3D_MESH3D_CLASS_ID);
    game3d_release_typed_ref(&cloth->owned_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    cloth->mesh = NULL;
    cloth->animator = NULL;
}

/// @brief Allocate a cloth with point/constraint storage; NULL plus a trap on failure.
/// @param points Fixed number of Verlet points to allocate.
/// @param constraints Fixed number of distance-constraint slots to allocate.
/// @param method Diagnostic text reported for object or array allocation failure.
/// @return Initialized runtime-managed cloth payload, or NULL after reporting allocation failure.
static rt_cloth3d *cloth3d_alloc(int32_t points, int32_t constraints, const char *method) {
    double *pos;
    double *prev;
    uint8_t *pinned;
    double *pin_pos;
    cloth3d_constraint *constraint_data;
    rt_cloth3d *cloth;
    if (points <= 0 || points > CLOTH3D_MAX_POINTS || constraints <= 0 ||
        (size_t)points > SIZE_MAX / (3u * sizeof(double)) ||
        (size_t)constraints > SIZE_MAX / sizeof(cloth3d_constraint)) {
        rt_trap(method);
        return NULL;
    }
    pos = (double *)calloc((size_t)points * 3u, sizeof(double));
    prev = (double *)calloc((size_t)points * 3u, sizeof(double));
    pinned = (uint8_t *)calloc((size_t)points, sizeof(uint8_t));
    pin_pos = (double *)calloc((size_t)points * 3u, sizeof(double));
    constraint_data = (cloth3d_constraint *)calloc((size_t)constraints, sizeof(cloth3d_constraint));
    if (!pos || !prev || !pinned || !pin_pos || !constraint_data) {
        free(pos);
        free(prev);
        free(pinned);
        free(pin_pos);
        free(constraint_data);
        rt_trap(method);
        return NULL;
    }
    cloth = (rt_cloth3d *)rt_obj_new_i64(RT_G3D_CLOTH3D_CLASS_ID, (int64_t)sizeof(*cloth));
    if (!cloth) {
        free(pos);
        free(prev);
        free(pinned);
        free(pin_pos);
        free(constraint_data);
        rt_trap(method);
        return NULL;
    }
    memset(cloth, 0, sizeof(*cloth));
    rt_obj_set_finalizer(cloth, cloth3d_finalize);
    cloth->pos = cloth->owned_pos = pos;
    cloth->prev = cloth->owned_prev = prev;
    cloth->pinned = cloth->owned_pinned = pinned;
    cloth->pin_pos = cloth->owned_pin_pos = pin_pos;
    cloth->constraints = cloth->owned_constraints = constraint_data;
    cloth->point_count = points;
    cloth->allocated_point_count = points;
    cloth->constraint_count = constraints;
    cloth->allocated_constraint_count = constraints;
    cloth->damping = 0.02;
    cloth->gravity_scale = 1.0;
    cloth->wind_response = 1.0;
    cloth->iterations = 4;
    cloth->substep_dt = 1.0 / 120.0;
    return cloth;
}

/// @brief Create a chain of segments+1 points hanging down -Y from the origin.
/// @param segments Number of equal-length links in the inclusive range 1 through 256.
/// @param total_length Positive total chain length.
/// @return Newly allocated Cloth3D chain, or NULL after reporting invalid input or allocation
/// failure.
void *rt_cloth3d_new_chain(int64_t segments, double total_length) {
    if (segments < 1 || segments > CLOTH3D_MAX_SEGMENTS) {
        rt_trap("Cloth3D.NewChain: segments must be 1..256");
        return NULL;
    }
    if (!isfinite(total_length) || total_length <= 0.0 || total_length > RT_GAME3D_COORD_ABS_MAX) {
        rt_trap("Cloth3D.NewChain: totalLength must be positive and within the coordinate range");
        return NULL;
    }
    int32_t points = (int32_t)segments + 1;
    rt_cloth3d *cloth =
        cloth3d_alloc(points, (int32_t)segments, "Cloth3D.NewChain: allocation failed");
    if (!cloth)
        return NULL;
    cloth->width = 1;
    cloth->height = points;
    cloth->topology_width = 1;
    cloth->topology_height = points;
    double seg = total_length / (double)segments;
    for (int32_t i = 0; i < points; ++i) {
        cloth->pos[i * 3 + 1] = -seg * (double)i;
        cloth->prev[i * 3 + 1] = cloth->pos[i * 3 + 1];
    }
    for (int32_t i = 0; i < (int32_t)segments; ++i) {
        cloth->constraints[i].a = i;
        cloth->constraints[i].b = i + 1;
        cloth->constraints[i].rest = seg;
    }
    cloth->constraint_count = (int32_t)segments;
    cloth->total_rest = total_length;
    return cloth;
}

/// @brief Create a w x h point grid in the XY plane (X right, Y down from origin).
/// @param w Number of point columns in the inclusive range 2 through 64.
/// @param h Number of point rows in the inclusive range 2 through 64.
/// @param width Positive horizontal span in simulation units.
/// @param height Positive vertical span in simulation units.
/// @return Newly allocated Cloth3D patch, or NULL after reporting invalid input or allocation
/// failure.
void *rt_cloth3d_new_patch(int64_t w, int64_t h, double width, double height) {
    if (w < 2 || h < 2 || w > CLOTH3D_MAX_PATCH_DIM || h > CLOTH3D_MAX_PATCH_DIM) {
        rt_trap("Cloth3D.NewPatch: grid dims must be 2..64");
        return NULL;
    }
    if (!isfinite(width) || width <= 0.0 || width > RT_GAME3D_COORD_ABS_MAX || !isfinite(height) ||
        height <= 0.0 || height > RT_GAME3D_COORD_ABS_MAX) {
        rt_trap("Cloth3D.NewPatch: size must be positive and within the coordinate range");
        return NULL;
    }
    int32_t points = (int32_t)(w * h);
    /* structural: (w-1)*h + w*(h-1); shear: 2*(w-1)*(h-1) */
    int32_t constraints = (int32_t)((w - 1) * h + w * (h - 1) + 2 * (w - 1) * (h - 1));
    rt_cloth3d *cloth = cloth3d_alloc(points, constraints, "Cloth3D.NewPatch: allocation failed");
    if (!cloth)
        return NULL;
    cloth->width = (int32_t)w;
    cloth->height = (int32_t)h;
    cloth->topology_width = (int32_t)w;
    cloth->topology_height = (int32_t)h;
    double dx = width / (double)(w - 1);
    double dy = height / (double)(h - 1);
    for (int32_t iy = 0; iy < (int32_t)h; ++iy) {
        for (int32_t ix = 0; ix < (int32_t)w; ++ix) {
            int32_t p = iy * (int32_t)w + ix;
            cloth->pos[p * 3 + 0] = width * ((double)ix / (double)(w - 1));
            cloth->pos[p * 3 + 1] = -height * ((double)iy / (double)(h - 1));
            memcpy(&cloth->prev[p * 3], &cloth->pos[p * 3], 3 * sizeof(double));
        }
    }
    int32_t c = 0;
    double diag = hypot(dx, dy);
    for (int32_t iy = 0; iy < (int32_t)h; ++iy) {
        for (int32_t ix = 0; ix < (int32_t)w; ++ix) {
            int32_t p = iy * (int32_t)w + ix;
            if (ix + 1 < (int32_t)w) {
                cloth->constraints[c].a = p;
                cloth->constraints[c].b = p + 1;
                cloth->constraints[c].rest = dx;
                ++c;
            }
            if (iy + 1 < (int32_t)h) {
                cloth->constraints[c].a = p;
                cloth->constraints[c].b = p + (int32_t)w;
                cloth->constraints[c].rest = dy;
                ++c;
            }
            if (ix + 1 < (int32_t)w && iy + 1 < (int32_t)h) {
                cloth->constraints[c].a = p;
                cloth->constraints[c].b = p + (int32_t)w + 1;
                cloth->constraints[c].rest = diag;
                ++c;
                cloth->constraints[c].a = p + 1;
                cloth->constraints[c].b = p + (int32_t)w;
                cloth->constraints[c].rest = diag;
                ++c;
            }
        }
    }
    cloth->constraint_count = c;
    cloth->total_rest = height;
    return cloth;
}

/// @brief Read the per-substep Verlet velocity damping fraction.
/// @param obj Cloth3D handle to inspect.
/// @return Damping in the inclusive range zero to one, or zero after invalid-handle reporting.
double rt_cloth3d_get_damping(void *obj) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.get_Damping: invalid cloth");
    return cloth ? cloth->damping : 0.0;
}

/// @brief Set the per-substep Verlet velocity damping fraction.
/// @param obj Cloth3D handle to modify.
/// @param damping Finite fraction clamped to the inclusive range zero to one.
void rt_cloth3d_set_damping(void *obj, double damping) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.set_Damping: invalid cloth");
    if (cloth && isfinite(damping))
        cloth->damping = damping < 0.0 ? 0.0 : (damping > 1.0 ? 1.0 : damping);
}

/// @brief Read the number of constraint-relaxation passes per fixed substep.
/// @param obj Cloth3D handle to inspect.
/// @return Configured iteration count, or zero after invalid-handle reporting.
int64_t rt_cloth3d_get_iterations(void *obj) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.get_Iterations: invalid cloth");
    return cloth ? cloth->iterations : 0;
}

/// @brief Set the number of constraint-relaxation passes per fixed substep.
/// @param obj Cloth3D handle to modify.
/// @param iterations Positive pass count, capped at 32; non-positive values are ignored.
void rt_cloth3d_set_iterations(void *obj, int64_t iterations) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.set_Iterations: invalid cloth");
    if (cloth && iterations >= 1)
        cloth->iterations = iterations > 32 ? 32 : (int32_t)iterations;
}

/// @brief Read the multiplier applied to the built-in downward gravity acceleration.
/// @param obj Cloth3D handle to inspect.
/// @return Configured gravity scale, or zero after invalid-handle reporting.
double rt_cloth3d_get_gravity_scale(void *obj) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.get_GravityScale: invalid cloth");
    return cloth ? cloth->gravity_scale : 0.0;
}

/// @brief Set the multiplier applied to the built-in downward gravity acceleration.
/// @param obj Cloth3D handle to modify.
/// @param scale Finite multiplier; non-finite values are ignored.
void rt_cloth3d_set_gravity_scale(void *obj, double scale) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.set_GravityScale: invalid cloth");
    if (cloth && isfinite(scale)) {
        if (scale > CLOTH3D_MAX_GRAVITY_SCALE)
            scale = CLOTH3D_MAX_GRAVITY_SCALE;
        if (scale < -CLOTH3D_MAX_GRAVITY_SCALE)
            scale = -CLOTH3D_MAX_GRAVITY_SCALE;
        cloth->gravity_scale = scale;
    }
}

/// @brief Read the coefficient that drives point velocity toward the configured wind vector.
/// @param obj Cloth3D handle to inspect.
/// @return Non-negative wind-response coefficient, or zero after invalid-handle reporting.
double rt_cloth3d_get_wind_response(void *obj) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.get_WindResponse: invalid cloth");
    return cloth ? cloth->wind_response : 0.0;
}

/// @brief Set the coefficient that drives point velocity toward the configured wind vector.
/// @param obj Cloth3D handle to modify.
/// @param response Finite non-negative coefficient; invalid values are ignored.
void rt_cloth3d_set_wind_response(void *obj, double response) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.set_WindResponse: invalid cloth");
    if (cloth && isfinite(response) && response >= 0.0)
        cloth->wind_response =
            response > CLOTH3D_MAX_WIND_RESPONSE ? CLOTH3D_MAX_WIND_RESPONSE : response;
}

/// @brief Read the fixed number of simulated points in a cloth.
/// @param obj Cloth3D handle to inspect.
/// @return Point count, or zero after invalid-handle reporting.
int64_t rt_cloth3d_get_point_count(void *obj) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.get_PointCount: invalid cloth");
    return cloth ? cloth->point_count : 0;
}

/// @brief Fluent: pin point @p index at its current position.
/// @param obj Cloth3D handle to modify.
/// @param index Zero-based point index.
/// @return The original @p obj handle, including after an invalid index is reported.
void *rt_cloth3d_pin(void *obj, int64_t index) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.Pin: invalid cloth");
    if (!cloth)
        return obj;
    if (index < 0 || index >= cloth->point_count) {
        rt_trap("Cloth3D.Pin: point index out of range");
        return obj;
    }
    cloth->pinned[index] = 1;
    memcpy(&cloth->pin_pos[index * 3], &cloth->pos[index * 3], 3 * sizeof(double));
    return obj;
}

/// @brief Reserve the next fixed-capacity collider slot.
/// @param cloth Valid Cloth3D payload whose collider count is incremented.
/// @param method Diagnostic text reported when the 16-collider budget is exhausted.
/// @return Reserved zero-based slot, or -1 after reporting budget exhaustion.
static int cloth3d_push_collider(rt_cloth3d *cloth, const char *method) {
    if (!cloth)
        return -1;
    cloth3d_repair_storage(cloth);
    if (cloth->collider_count >= CLOTH3D_MAX_COLLIDERS) {
        rt_trap(method);
        return -1;
    }
    cloth->initialized_collider_count = cloth->collider_count + 1;
    return cloth->collider_count++;
}

/// @brief Fluent: add a static sphere collider.
/// @param obj Cloth3D handle to modify.
/// @param center Vec3 world-space sphere center.
/// @param radius Positive collision radius.
/// @return The original @p obj handle; invalid input or budget exhaustion is reported by a trap.
void *rt_cloth3d_add_sphere(void *obj, void *center, double radius) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.AddSphere: invalid cloth");
    double c[3];
    if (!cloth)
        return obj;
    if (!game3d_read_vec3(center, c, "Cloth3D.AddSphere: center must be Vec3"))
        return obj;
    if (!isfinite(radius) || radius <= 0.0 || radius > RT_GAME3D_COORD_ABS_MAX) {
        rt_trap("Cloth3D.AddSphere: radius must be positive and within the coordinate range");
        return obj;
    }
    int slot = cloth3d_push_collider(cloth, "Cloth3D.AddSphere: collider budget (16) exceeded");
    if (slot < 0)
        return obj;
    cloth->colliders[slot].is_capsule = 0;
    memcpy(cloth->colliders[slot].a, c, sizeof(c));
    cloth->colliders[slot].radius = radius;
    return obj;
}

/// @brief Fluent: add a static capsule collider (segment a..b).
/// @param obj Cloth3D handle to modify.
/// @param a_obj Vec3 world-space capsule segment start.
/// @param b_obj Vec3 world-space capsule segment end.
/// @param radius Positive collision radius surrounding the segment.
/// @return The original @p obj handle; invalid input or budget exhaustion is reported by a trap.
void *rt_cloth3d_add_capsule(void *obj, void *a_obj, void *b_obj, double radius) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.AddCapsule: invalid cloth");
    double a[3], b[3];
    if (!cloth)
        return obj;
    if (!game3d_read_vec3(a_obj, a, "Cloth3D.AddCapsule: a must be Vec3") ||
        !game3d_read_vec3(b_obj, b, "Cloth3D.AddCapsule: b must be Vec3"))
        return obj;
    if (!isfinite(radius) || radius <= 0.0 || radius > RT_GAME3D_COORD_ABS_MAX) {
        rt_trap("Cloth3D.AddCapsule: radius must be positive and within the coordinate range");
        return obj;
    }
    int slot = cloth3d_push_collider(cloth, "Cloth3D.AddCapsule: collider budget (16) exceeded");
    if (slot < 0)
        return obj;
    cloth->colliders[slot].is_capsule = 1;
    memcpy(cloth->colliders[slot].a, a, sizeof(a));
    memcpy(cloth->colliders[slot].b, b, sizeof(b));
    cloth->colliders[slot].radius = radius;
    return obj;
}

/// @brief Set the wind velocity (direction Vec3 scaled by strength).
/// @param obj Cloth3D handle to modify.
/// @param direction Vec3 direction and relative per-axis magnitude.
/// @param strength Scalar multiplier; a non-finite value is normalized to zero.
void rt_cloth3d_set_wind(void *obj, void *direction, double strength) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.SetWind: invalid cloth");
    double d[3];
    if (!cloth)
        return;
    if (!game3d_read_vec3(direction, d, "Cloth3D.SetWind: direction must be Vec3"))
        return;
    if (!isfinite(strength))
        strength = 0.0;
    if (strength > CLOTH3D_MAX_WIND_STRENGTH)
        strength = CLOTH3D_MAX_WIND_STRENGTH;
    if (strength < -CLOTH3D_MAX_WIND_STRENGTH)
        strength = -CLOTH3D_MAX_WIND_STRENGTH;
    for (int i = 0; i < 3; ++i)
        cloth->wind[i] = cloth3d_coord_or(d[i] * strength, 0.0);
}

/// @brief Current position of one point as a Vec3.
/// @param obj Cloth3D handle to inspect.
/// @param index Zero-based point index.
/// @return Newly allocated Vec3 containing the point position, or the origin for an invalid index
/// or handle.
void *rt_cloth3d_get_point(void *obj, int64_t index) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.GetPoint: invalid cloth");
    if (!cloth || index < 0 || index >= cloth->point_count)
        return rt_vec3_new(0.0, 0.0, 0.0);
    for (int lane = 0; lane < 3; ++lane) {
        double fallback = cloth->pinned[index] ? cloth->pin_pos[index * 3 + lane] : 0.0;
        cloth->pos[index * 3 + lane] = cloth3d_coord_or(cloth->pos[index * 3 + lane], fallback);
        cloth->prev[index * 3 + lane] =
            cloth3d_coord_or(cloth->prev[index * 3 + lane], cloth->pos[index * 3 + lane]);
    }
    return rt_vec3_new(cloth->pos[index * 3], cloth->pos[index * 3 + 1], cloth->pos[index * 3 + 2]);
}

/// @brief Fluent: bind a patch to a Mesh3D (built once here, rewritten per step).
/// @param obj Patch Cloth3D handle to bind.
/// @param mesh Mesh3D handle retained by the cloth and rebuilt with grid topology.
/// @return The original @p obj handle; incompatible cloth or mesh inputs are reported by a trap.
void *rt_cloth3d_bind_mesh(void *obj, void *mesh) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.BindMesh: invalid cloth");
    rt_mesh3d *mesh_data;
    int32_t triangle_count;
    if (!cloth)
        return obj;
    if (cloth->width < 2) {
        rt_trap("Cloth3D.BindMesh: mesh binding requires a patch cloth");
        return obj;
    }
    if (!rt_g3d_has_class(mesh, RT_G3D_MESH3D_CLASS_ID)) {
        rt_trap("Cloth3D.BindMesh: expected Mesh3D");
        return obj;
    }
    mesh_data = (rt_mesh3d *)mesh;
    int32_t w = cloth->width, h = cloth->height;
    triangle_count = (w - 1) * (h - 1) * 2;
    for (int32_t p = 0; p < cloth->point_count; ++p) {
        for (int lane = 0; lane < 3; ++lane) {
            double value = cloth->pos[p * 3 + lane];
            if (!isfinite(value) || value < -(double)FLT_MAX || value > (double)FLT_MAX) {
                rt_trap("Cloth3D.BindMesh: point coordinates must fit float storage");
                return obj;
            }
        }
    }
    rt_mesh3d_reserve(mesh, cloth->point_count, triangle_count);
    if (mesh_data->vertex_capacity < (uint32_t)cloth->point_count ||
        mesh_data->index_capacity < (uint32_t)triangle_count * 3u) {
        rt_trap("Cloth3D.BindMesh: mesh reserve failed");
        return obj;
    }
    rt_mesh3d_begin_geometry_batch(mesh_data);
    rt_mesh3d_clear(mesh);
    for (int32_t p = 0; p < cloth->point_count; ++p)
        rt_mesh3d_add_vertex(mesh,
                             cloth->pos[p * 3],
                             cloth->pos[p * 3 + 1],
                             cloth->pos[p * 3 + 2],
                             0.0,
                             0.0,
                             1.0,
                             (double)(p % w) / (double)(w - 1),
                             (double)(p / w) / (double)(h - 1));
    for (int32_t iy = 0; iy + 1 < h; ++iy) {
        for (int32_t ix = 0; ix + 1 < w; ++ix) {
            int32_t p = iy * w + ix;
            rt_mesh3d_add_triangle(mesh, p, p + w, p + 1);
            rt_mesh3d_add_triangle(mesh, p + 1, p + w, p + w + 1);
        }
    }
    rt_mesh3d_end_geometry_batch(mesh_data);
    if (mesh_data->build_failed || mesh_data->vertex_count != (uint32_t)cloth->point_count ||
        mesh_data->index_count != (uint32_t)triangle_count * 3u) {
        rt_trap("Cloth3D.BindMesh: mesh construction failed");
        return obj;
    }
    cloth3d_assign_owned_ref(&cloth->owned_mesh, &cloth->mesh, mesh, RT_G3D_MESH3D_CLASS_ID);
    return obj;
}

/// @brief Fluent: bind a chain cloth to an animator's linear bone chain.
/// @details Walks single-child links from @p root_bone (branching traps),
///   reseeds rest lengths from the bind pose, and pins point 0 to the root
///   bone's animated model-space position each step.
/// @param obj Chain Cloth3D handle to bind.
/// @param animator AnimController3D handle with a skeleton, retained by the cloth.
/// @param root_bone Name of the first bone in the required linear descendant chain.
/// @return The original @p obj handle; invalid, branching, or allocation cases report a trap.
void *rt_cloth3d_bind_bone_chain(void *obj, void *animator, rt_string root_bone) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.BindBoneChain: invalid cloth");
    int32_t staged_bones[CLOTH3D_MAX_CHAIN_BONES];
    double staged_rest[CLOTH3D_MAX_CHAIN_BONES];
    int32_t staged_bone_count = 0;
    int8_t *new_override_mask;
    float *new_override_globals;
    if (!cloth)
        return obj;
    if (cloth->width != 1) {
        rt_trap("Cloth3D.BindBoneChain: bone binding requires a chain cloth");
        return obj;
    }
    if (!rt_g3d_has_class(animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID)) {
        rt_trap("Cloth3D.BindBoneChain: expected AnimController3D with a skeleton");
        return obj;
    }
    if (!root_bone || !rt_string_is_handle(root_bone)) {
        rt_trap("Cloth3D.BindBoneChain: root bone must be a runtime string");
        return obj;
    }
    void *skeleton = rt_anim_controller3d_get_skeleton(animator);
    if (!skeleton) {
        rt_trap("Cloth3D.BindBoneChain: expected AnimController3D with a skeleton");
        return obj;
    }
    int64_t root = rt_skeleton3d_find_bone(skeleton, root_bone);
    if (root < 0) {
        rt_trap("Cloth3D.BindBoneChain: root bone not found");
        return obj;
    }
    int64_t bone_count = rt_skeleton3d_get_bone_count(skeleton);
    int64_t current = root;
    while (current >= 0) {
        if (staged_bone_count >= CLOTH3D_MAX_CHAIN_BONES) {
            rt_trap("Cloth3D.BindBoneChain: bone chain exceeds the 32-bone limit");
            return obj;
        }
        staged_bones[staged_bone_count++] = (int32_t)current;
        int64_t child = -1;
        for (int64_t i = 0; i < bone_count; ++i) {
            if (rt_skeleton3d_get_bone_parent_raw(skeleton, i) != current)
                continue;
            if (child >= 0) {
                rt_trap("Cloth3D.BindBoneChain: branching bone chains are not supported");
                return obj;
            }
            child = i;
        }
        current = child;
    }
    if (staged_bone_count < 1) {
        rt_trap("Cloth3D.BindBoneChain: empty bone chain");
        return obj;
    }
    /* One point per bone plus a tip; reseed rest lengths from bind locals. */
    int32_t needed = staged_bone_count + 1;
    if (needed > cloth->point_count)
        needed = cloth->point_count;
    double local[16];
    for (int32_t i = 0; i + 1 < needed; ++i) {
        double rest = cloth->constraints[i].rest;
        if (!isfinite(rest) || rest <= 0.0)
            rest = cloth->total_rest / (double)(cloth->point_count - 1);
        if (i + 1 < staged_bone_count &&
            rt_skeleton3d_get_bone_bind_local_raw(skeleton, staged_bones[i + 1], local)) {
            /* Row-major bind local: translation in elements 3, 7, 11. */
            double tx = local[3], ty = local[7], tz = local[11];
            double translation[3] = {tx, ty, tz};
            double len = cloth3d_length3(translation);
            if (isfinite(len) && len > 1e-6)
                rest = len;
        }
        staged_rest[i] = cloth3d_coord_or(rest, 1.0);
    }
    new_override_mask = (int8_t *)calloc((size_t)bone_count, sizeof(int8_t));
    new_override_globals = (float *)calloc((size_t)bone_count * 16u, sizeof(float));
    if (!new_override_mask || !new_override_globals) {
        free(new_override_mask);
        free(new_override_globals);
        rt_trap("Cloth3D.BindBoneChain: allocation failed");
        return obj;
    }
    cloth3d_assign_owned_ref(
        &cloth->owned_animator, &cloth->animator, animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    free(cloth->owned_override_mask);
    free(cloth->owned_override_globals);
    cloth->owned_override_mask = cloth->override_mask = new_override_mask;
    cloth->owned_override_globals = cloth->override_globals = new_override_globals;
    cloth->allocated_skeleton_bone_count = cloth->skeleton_bone_count = (int32_t)bone_count;
    memcpy(cloth->chain_bones, staged_bones, (size_t)staged_bone_count * sizeof(staged_bones[0]));
    cloth->initialized_chain_bone_count = cloth->chain_bone_count = staged_bone_count;
    for (int32_t i = 0; i + 1 < needed; ++i)
        cloth->constraints[i].rest = staged_rest[i];
    cloth->total_rest = cloth3d_sum_rest_lengths(cloth);
    cloth->pinned[0] = 1;
    cloth->anchor_initialized = 0;
    return obj;
}

/*==========================================================================
 * Simulation
 *=========================================================================*/

/// @brief Repair one Verlet point to finite bounded state before arithmetic.
/// @param cloth Cloth containing the point.
/// @param point Valid point index.
static void cloth3d_repair_point(rt_cloth3d *cloth, int32_t point) {
    if (!cloth || point < 0 || point >= cloth->point_count)
        return;
    cloth->pinned[point] = cloth->pinned[point] ? 1u : 0u;
    for (int lane = 0; lane < 3; ++lane) {
        size_t index = (size_t)point * 3u + (size_t)lane;
        double fallback = cloth->pinned[point] ? cloth->pin_pos[index] : 0.0;
        cloth->pin_pos[index] = cloth3d_coord_or(cloth->pin_pos[index], 0.0);
        cloth->pos[index] = cloth3d_coord_or(cloth->pos[index], fallback);
        cloth->prev[index] = cloth3d_coord_or(cloth->prev[index], cloth->pos[index]);
    }
}

/// @brief Compute the closest point on a bounded capsule axis without squared-length overflow.
/// @param a Segment start.
/// @param b Segment end.
/// @param point Query point.
/// @param[out] closest Closest point on the closed segment.
static void cloth3d_closest_on_segment(const double a[3],
                                       const double b[3],
                                       const double point[3],
                                       double closest[3]) {
    double axis[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    double length = cloth3d_length3(axis);
    double distance = 0.0;
    if (isfinite(length) && length > 1e-12) {
        for (int lane = 0; lane < 3; ++lane)
            axis[lane] /= length;
        distance =
            (point[0] - a[0]) * axis[0] + (point[1] - a[1]) * axis[1] + (point[2] - a[2]) * axis[2];
        if (!isfinite(distance) || distance < 0.0)
            distance = 0.0;
        if (distance > length)
            distance = length;
    }
    for (int lane = 0; lane < 3; ++lane)
        closest[lane] = cloth3d_coord_or(a[lane] + axis[lane] * distance, a[lane]);
}

/// @brief One fixed substep: integrate, relax constraints, push out, re-pin.
/// @param cloth Valid mutable cloth payload to advance by its configured fixed interval.
static void cloth3d_substep(rt_cloth3d *cloth) {
    double dt = cloth->substep_dt;
    double dt2 = dt * dt;
    double keep = 1.0 - cloth->damping;
    double gravity = -CLOTH3D_GRAVITY * cloth->gravity_scale;
    double wind_coupling = cloth->wind_response * dt2;
    double displacement_factor = keep - cloth->wind_response * dt;
    for (int32_t p = 0; p < cloth->point_count; ++p) {
        cloth3d_repair_point(cloth, p);
        if (cloth->pinned[p])
            continue;
        double *pos = &cloth->pos[p * 3];
        double *prev = &cloth->prev[p * 3];
        for (int i = 0; i < 3; ++i) {
            double displacement = pos[i] - prev[i];
            double acceleration_term = cloth->wind[i] * wind_coupling;
            double next;
            if (i == 1)
                acceleration_term += gravity * dt2;
            next = pos[i] + displacement * displacement_factor + acceleration_term;
            prev[i] = pos[i];
            pos[i] = cloth3d_coord_or(next, prev[i]);
        }
    }
    for (int32_t it = 0; it < cloth->iterations; ++it) {
        for (int32_t c = 0; c < cloth->constraint_count; ++c) {
            cloth3d_constraint *con = &cloth->constraints[c];
            if (con->a < 0 || con->a >= cloth->point_count || con->b < 0 ||
                con->b >= cloth->point_count || con->a == con->b || !isfinite(con->rest) ||
                con->rest <= 0.0)
                continue;
            double *pa = &cloth->pos[con->a * 3];
            double *pb = &cloth->pos[con->b * 3];
            double d[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
            double len = cloth3d_length3(d);
            if (!isfinite(len) || len < 1e-9)
                continue;
            double correction = len - con->rest;
            double direction[3] = {d[0] / len, d[1] / len, d[2] / len};
            /* Distribute the correction so wa + wb == 1: an unpinned point
             * facing a pinned partner absorbs the whole correction; two free
             * points split it evenly; two pinned points cannot move. */
            if (cloth->pinned[con->a] && cloth->pinned[con->b])
                continue;
            double wa = cloth->pinned[con->a] ? 0.0 : (cloth->pinned[con->b] ? 1.0 : 0.5);
            double wb = cloth->pinned[con->b] ? 0.0 : 1.0 - wa;
            for (int i = 0; i < 3; ++i) {
                pa[i] = cloth3d_coord_or(pa[i] + direction[i] * correction * wa, pa[i]);
                pb[i] = cloth3d_coord_or(pb[i] - direction[i] * correction * wb, pb[i]);
            }
        }
    }
    for (int32_t k = 0; k < cloth->collider_count; ++k) {
        cloth3d_collider *col = &cloth->colliders[k];
        for (int32_t p = 0; p < cloth->point_count; ++p) {
            if (cloth->pinned[p])
                continue;
            double *pos = &cloth->pos[p * 3];
            double closest[3];
            if (col->is_capsule)
                cloth3d_closest_on_segment(col->a, col->b, pos, closest);
            else
                memcpy(closest, col->a, sizeof(closest));
            double d[3] = {pos[0] - closest[0], pos[1] - closest[1], pos[2] - closest[2]};
            double dist = cloth3d_length3(d);
            if (!isfinite(dist))
                continue;
            if (dist >= col->radius)
                continue;
            if (dist < 1e-9) {
                /* Point sits exactly on the collider center/axis: eject along a
                 * direction derived from the collider — perpendicular to a
                 * capsule's axis, or toward the point's previous position for a
                 * sphere — rather than a fixed world axis. */
                double eject[3] = {0.0, 1.0, 0.0};
                const double *prev = &cloth->prev[p * 3];
                double away[3] = {prev[0] - closest[0], prev[1] - closest[1], prev[2] - closest[2]};
                double away_len = cloth3d_length3(away);
                if (isfinite(away_len) && away_len > 1e-9) {
                    eject[0] = away[0] / away_len;
                    eject[1] = away[1] / away_len;
                    eject[2] = away[2] / away_len;
                }
                if (col->is_capsule) {
                    /* Remove the axis-parallel component so the point leaves the
                     * capsule through its side, the nearest surface. */
                    double ab[3] = {
                        col->b[0] - col->a[0], col->b[1] - col->a[1], col->b[2] - col->a[2]};
                    double ab_len = cloth3d_length3(ab);
                    if (isfinite(ab_len) && ab_len > 1e-9) {
                        double axis[3] = {ab[0] / ab_len, ab[1] / ab_len, ab[2] / ab_len};
                        double along = eject[0] * axis[0] + eject[1] * axis[1] + eject[2] * axis[2];
                        double perp[3] = {eject[0] - axis[0] * along,
                                          eject[1] - axis[1] * along,
                                          eject[2] - axis[2] * along};
                        double perp_len = cloth3d_length3(perp);
                        if (isfinite(perp_len) && perp_len > 1e-9) {
                            eject[0] = perp[0] / perp_len;
                            eject[1] = perp[1] / perp_len;
                            eject[2] = perp[2] / perp_len;
                        } else {
                            /* Previous position lies on the axis too: pick any
                             * unit vector perpendicular to the axis. */
                            double basis[3] = {1.0, 0.0, 0.0};
                            if (fabs(axis[0]) > 0.9)
                                basis[0] = 0.0, basis[1] = 1.0;
                            eject[0] = axis[1] * basis[2] - axis[2] * basis[1];
                            eject[1] = axis[2] * basis[0] - axis[0] * basis[2];
                            eject[2] = axis[0] * basis[1] - axis[1] * basis[0];
                            double el = cloth3d_length3(eject);
                            if (isfinite(el) && el > 1e-9) {
                                eject[0] /= el;
                                eject[1] /= el;
                                eject[2] /= el;
                            } else {
                                eject[0] = 0.0;
                                eject[1] = 1.0;
                                eject[2] = 0.0;
                            }
                        }
                    }
                }
                pos[0] = cloth3d_coord_or(closest[0] + eject[0] * col->radius, closest[0]);
                pos[1] = cloth3d_coord_or(closest[1] + eject[1] * col->radius, closest[1]);
                pos[2] = cloth3d_coord_or(closest[2] + eject[2] * col->radius, closest[2]);
                continue;
            }
            double push = col->radius;
            for (int i = 0; i < 3; ++i)
                pos[i] = cloth3d_coord_or(closest[i] + (d[i] / dist) * push, closest[i]);
        }
    }
    for (int32_t p = 0; p < cloth->point_count; ++p) {
        if (!cloth->pinned[p])
            continue;
        for (int lane = 0; lane < 3; ++lane) {
            double target = cloth3d_coord_or(cloth->pin_pos[p * 3 + lane], 0.0);
            cloth->pin_pos[p * 3 + lane] = target;
            cloth->pos[p * 3 + lane] = target;
            cloth->prev[p * 3 + lane] = target;
        }
    }
}

/// @brief Quaternion rotating unit vector @p a onto unit vector @p b.
/// @param a Normalized source direction.
/// @param b Normalized destination direction.
/// @param out Receives the normalized XYZW rotation quaternion.
static void cloth3d_quat_from_to(const double a[3], const double b[3], double out[4]) {
    double from[3] = {a ? a[0] : 0.0, a ? a[1] : 0.0, a ? a[2] : 0.0};
    double to[3] = {b ? b[0] : 0.0, b ? b[1] : 0.0, b ? b[2] : 0.0};
    double from_len = cloth3d_length3(from);
    double to_len = cloth3d_length3(to);
    double dot;
    if (!out)
        return;
    if (!isfinite(from_len) || from_len <= 1e-12 || !isfinite(to_len) || to_len <= 1e-12) {
        out[0] = out[1] = out[2] = 0.0;
        out[3] = 1.0;
        return;
    }
    for (int lane = 0; lane < 3; ++lane) {
        from[lane] /= from_len;
        to[lane] /= to_len;
    }
    dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
    if (dot > 1.0)
        dot = 1.0;
    if (dot < -1.0)
        dot = -1.0;
    if (dot > 1.0 - 1e-9) {
        out[0] = out[1] = out[2] = 0.0;
        out[3] = 1.0;
        return;
    }
    if (dot < -1.0 + 1e-9) {
        /* Antiparallel: rotate pi around any axis orthogonal to a. */
        double axis[3] = {-from[1], from[0], 0.0};
        double len = hypot(axis[0], axis[1]);
        if (len < 1e-6) {
            axis[0] = 0.0;
            axis[1] = -from[2];
            axis[2] = from[1];
            len = hypot(axis[1], axis[2]);
        }
        if (!isfinite(len) || len <= 1e-12) {
            out[0] = 1.0;
            out[1] = out[2] = out[3] = 0.0;
            return;
        }
        out[0] = axis[0] / len;
        out[1] = axis[1] / len;
        out[2] = axis[2] / len;
        out[3] = 0.0;
        return;
    }
    double cross[3] = {from[1] * to[2] - from[2] * to[1],
                       from[2] * to[0] - from[0] * to[2],
                       from[0] * to[1] - from[1] * to[0]};
    out[0] = cross[0];
    out[1] = cross[1];
    out[2] = cross[2];
    out[3] = 1.0 + dot;
    double norm = hypot(hypot(out[0], out[1]), hypot(out[2], out[3]));
    if (!isfinite(norm) || norm <= 1e-12) {
        out[0] = out[1] = out[2] = 0.0;
        out[3] = 1.0;
        return;
    }
    for (int i = 0; i < 4; ++i)
        out[i] /= norm;
}

/// @brief Normalize a quaternion robustly, falling back to identity.
/// @param[in,out] quaternion XYZW quaternion.
static void cloth3d_quat_normalize(double quaternion[4]) {
    double max_abs;
    double norm;
    if (!quaternion)
        return;
    max_abs = fmax(fmax(fabs(quaternion[0]), fabs(quaternion[1])),
                   fmax(fabs(quaternion[2]), fabs(quaternion[3])));
    if (!isfinite(max_abs) || max_abs <= 1e-12) {
        quaternion[0] = quaternion[1] = quaternion[2] = 0.0;
        quaternion[3] = 1.0;
        return;
    }
    norm = hypot(hypot(quaternion[0] / max_abs, quaternion[1] / max_abs),
                 hypot(quaternion[2] / max_abs, quaternion[3] / max_abs));
    if (!isfinite(norm) || norm <= 1e-12) {
        quaternion[0] = quaternion[1] = quaternion[2] = 0.0;
        quaternion[3] = 1.0;
        return;
    }
    for (int lane = 0; lane < 4; ++lane)
        quaternion[lane] = (quaternion[lane] / max_abs) / norm;
}

/// @brief Hamilton product out = q1 * q2 (xyzw layout).
/// @param q1 Left-hand XYZW quaternion.
/// @param q2 Right-hand XYZW quaternion.
/// @param out Receives the XYZW Hamilton product.
static void cloth3d_quat_mul(const double q1[4], const double q2[4], double out[4]) {
    if (!q1 || !q2 || !out)
        return;
    out[0] = q1[3] * q2[0] + q1[0] * q2[3] + q1[1] * q2[2] - q1[2] * q2[1];
    out[1] = q1[3] * q2[1] - q1[0] * q2[2] + q1[1] * q2[3] + q1[2] * q2[0];
    out[2] = q1[3] * q2[2] + q1[0] * q2[1] - q1[1] * q2[0] + q1[2] * q2[3];
    out[3] = q1[3] * q2[3] - q1[0] * q2[0] - q1[1] * q2[1] - q1[2] * q2[2];
    cloth3d_quat_normalize(out);
}

/// @brief Column-layout 4x4 from quaternion + position (ragdoll override format).
/// @param q XYZW orientation quaternion.
/// @param p Translation vector.
/// @param m Receives the 16-element transform matrix in pose-override layout.
static void cloth3d_mat_from_quat_pos(const double q[4], const double p[3], double *m) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1.0 - 2.0 * (y * y + z * z);
    m[1] = 2.0 * (x * y + z * w);
    m[2] = 2.0 * (x * z - y * w);
    m[3] = 0.0;
    m[4] = 2.0 * (x * y - z * w);
    m[5] = 1.0 - 2.0 * (x * x + z * z);
    m[6] = 2.0 * (y * z + x * w);
    m[7] = 0.0;
    m[8] = 2.0 * (x * z + y * w);
    m[9] = 2.0 * (y * z - x * w);
    m[10] = 1.0 - 2.0 * (x * x + y * y);
    m[11] = 0.0;
    m[12] = p[0];
    m[13] = p[1];
    m[14] = p[2];
    m[15] = 1.0;
}

/// @brief Pin the chain anchor to the root bone's animated model-space pose.
/// @details Anchor jumps beyond half the chain's rest length (teleports,
///   including the very first bind sync) rigid-translate the whole cloth so
///   verlet never manufactures a huge phantom velocity from the pin snap.
/// @param cloth Chain cloth whose retained animator supplies the root pose.
static void cloth3d_sync_anchor(rt_cloth3d *cloth) {
    double pos[3], quat[4];
    if (!cloth || !cloth->animator || cloth->chain_bone_count < 1 || !cloth->pinned ||
        !cloth->pin_pos || !cloth->pos || !cloth->prev)
        return;
    if (!rt_g3d_has_class(cloth->animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID)) {
        cloth->animator = cloth->owned_animator;
        if (!rt_g3d_has_class(cloth->animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID))
            return;
    }
    if (!rt_anim_controller3d_get_bone_pose(cloth->animator, cloth->chain_bones[0], pos, quat))
        return;
    for (int lane = 0; lane < 3; ++lane) {
        if (!isfinite(pos[lane]))
            return;
        pos[lane] = cloth3d_coord_or(pos[lane], 0.0);
    }
    double delta[3] = {
        pos[0] - cloth->pin_pos[0], pos[1] - cloth->pin_pos[1], pos[2] - cloth->pin_pos[2]};
    double jump = cloth3d_length3(delta);
    double threshold = 0.5 * cloth->total_rest;
    if (!isfinite(threshold) || threshold < 0.25)
        threshold = 0.25;
    if (isfinite(jump) && (!cloth->anchor_initialized || jump > threshold)) {
        for (int32_t p = 0; p < cloth->point_count; ++p) {
            for (int i = 0; i < 3; ++i) {
                cloth->pos[p * 3 + i] = cloth3d_coord_or(cloth->pos[p * 3 + i] + delta[i], pos[i]);
                cloth->prev[p * 3 + i] =
                    cloth3d_coord_or(cloth->prev[p * 3 + i] + delta[i], pos[i]);
                if (cloth->pinned[p])
                    cloth->pin_pos[p * 3 + i] =
                        cloth3d_coord_or(cloth->pin_pos[p * 3 + i] + delta[i], pos[i]);
            }
        }
    }
    cloth->pinned[0] = 1;
    memcpy(&cloth->pin_pos[0], pos, sizeof(pos));
    cloth->anchor_initialized = 1;
}

/// @brief Write simulated chain directions back as bone aim rotations.
/// @param cloth Bound chain cloth whose pose-override buffers are populated and applied.
static void cloth3d_write_bone_overrides(rt_cloth3d *cloth) {
    if (!cloth->animator || cloth->chain_bone_count < 1 || !cloth->override_mask ||
        !cloth->override_globals || cloth->skeleton_bone_count <= 0)
        return;
    memset(cloth->override_mask, 0, (size_t)cloth->skeleton_bone_count);
    int32_t links = cloth->chain_bone_count;
    if (links > cloth->point_count - 1)
        links = cloth->point_count - 1;
    double next_anim_pos[3], next_anim_quat[4];
    for (int32_t i = 0; i < links; ++i) {
        int32_t bone = cloth->chain_bones[i];
        double anim_pos[3], anim_quat[4];
        if (bone < 0 || bone >= cloth->skeleton_bone_count)
            continue;
        if (!rt_anim_controller3d_get_bone_pose(cloth->animator, bone, anim_pos, anim_quat))
            return;
        double anim_dir[3];
        int have_anim_dir = 0;
        if (i + 1 < cloth->chain_bone_count &&
            rt_anim_controller3d_get_bone_pose(
                cloth->animator, cloth->chain_bones[i + 1], next_anim_pos, next_anim_quat)) {
            anim_dir[0] = next_anim_pos[0] - anim_pos[0];
            anim_dir[1] = next_anim_pos[1] - anim_pos[1];
            anim_dir[2] = next_anim_pos[2] - anim_pos[2];
            have_anim_dir = 1;
        } else {
            anim_dir[0] = 0.0;
            anim_dir[1] = -1.0;
            anim_dir[2] = 0.0;
            have_anim_dir = 1;
        }
        double sim_dir[3] = {cloth->pos[(i + 1) * 3] - cloth->pos[i * 3],
                             cloth->pos[(i + 1) * 3 + 1] - cloth->pos[i * 3 + 1],
                             cloth->pos[(i + 1) * 3 + 2] - cloth->pos[i * 3 + 2]};
        double alen = cloth3d_length3(anim_dir);
        double slen = cloth3d_length3(sim_dir);
        if (!have_anim_dir || !isfinite(alen) || alen < 1e-9 || !isfinite(slen) || slen < 1e-9)
            continue;
        for (int k = 0; k < 3; ++k) {
            anim_dir[k] /= alen;
            sim_dir[k] /= slen;
        }
        double delta[4], final_quat[4], global[16];
        cloth3d_quat_normalize(anim_quat);
        cloth3d_quat_from_to(anim_dir, sim_dir, delta);
        cloth3d_quat_mul(delta, anim_quat, final_quat);
        for (int lane = 0; lane < 3; ++lane)
            anim_pos[lane] = cloth3d_coord_or(anim_pos[lane], 0.0);
        cloth3d_mat_from_quat_pos(final_quat, anim_pos, global);
        float *dst = &cloth->override_globals[bone * 16];
        for (int k = 0; k < 16; ++k) {
            double value = global[k];
            if (!isfinite(value))
                value = (k % 5 == 0) ? 1.0 : 0.0;
            if (value > FLT_MAX)
                value = FLT_MAX;
            if (value < -FLT_MAX)
                value = -FLT_MAX;
            dst[k] = (float)value;
        }
        cloth->override_mask[bone] = 1;
    }
    rt_anim_controller3d_apply_pose_override(
        cloth->animator, cloth->override_mask, cloth->override_globals);
}

/// @brief Rewrite the bound mesh's vertices/normals in place from the grid.
/// @param cloth Bound patch cloth supplying point positions and grid topology.
static void cloth3d_write_mesh(rt_cloth3d *cloth) {
    rt_mesh3d *mesh = (rt_mesh3d *)cloth->mesh;
    int changed = 0;
    if (!mesh || !rt_g3d_has_class(mesh, RT_G3D_MESH3D_CLASS_ID))
        return;
    rt_mesh3d_repair_geometry_counts(mesh);
    int32_t w = cloth->width, h = cloth->height;
    if (w < 2 || h < 2 || (int64_t)w * h != cloth->point_count ||
        mesh->vertex_count < (uint32_t)cloth->point_count || !mesh->vertices)
        return;
    for (int32_t iy = 0; iy < h; ++iy) {
        for (int32_t ix = 0; ix < w; ++ix) {
            int32_t p = iy * w + ix;
            const double *pos = &cloth->pos[p * 3];
            /* Grid normal from central-difference tangents. */
            int32_t xr = ix + 1 < w ? p + 1 : p;
            int32_t xl = ix > 0 ? p - 1 : p;
            int32_t yd = iy + 1 < h ? p + w : p;
            int32_t yu = iy > 0 ? p - w : p;
            double tx[3] = {cloth->pos[xr * 3] - cloth->pos[xl * 3],
                            cloth->pos[xr * 3 + 1] - cloth->pos[xl * 3 + 1],
                            cloth->pos[xr * 3 + 2] - cloth->pos[xl * 3 + 2]};
            double ty[3] = {cloth->pos[yd * 3] - cloth->pos[yu * 3],
                            cloth->pos[yd * 3 + 1] - cloth->pos[yu * 3 + 1],
                            cloth->pos[yd * 3 + 2] - cloth->pos[yu * 3 + 2]};
            /* ty x tx (not tx x ty): the grid is X-right / Y-down (pos.y = -dy*iy),
             * so tx x ty points -Z at rest, opposite the seeded bind normal (+Z)
             * and the CCW triangle winding's front face. Use ty x tx for +Z. */
            double tx_len = cloth3d_length3(tx);
            double ty_len = cloth3d_length3(ty);
            if (isfinite(tx_len) && tx_len > 1e-12)
                for (int lane = 0; lane < 3; ++lane)
                    tx[lane] /= tx_len;
            if (isfinite(ty_len) && ty_len > 1e-12)
                for (int lane = 0; lane < 3; ++lane)
                    ty[lane] /= ty_len;
            double n[3] = {ty[1] * tx[2] - ty[2] * tx[1],
                           ty[2] * tx[0] - ty[0] * tx[2],
                           ty[0] * tx[1] - ty[1] * tx[0]};
            double nlen = cloth3d_length3(n);
            if (!isfinite(nlen) || nlen < 1e-12) {
                n[0] = 0.0;
                n[1] = 0.0;
                n[2] = 1.0;
                nlen = 1.0;
            }
            vgfx3d_vertex_t *vertex = &mesh->vertices[p];
            float next_pos[3];
            float next_normal[3];
            for (int lane = 0; lane < 3; ++lane) {
                double finite_pos = cloth3d_coord_or(pos[lane], 0.0);
                next_pos[lane] = (float)finite_pos;
                next_normal[lane] = (float)(n[lane] / nlen);
                if (vertex->pos[lane] != next_pos[lane] ||
                    vertex->normal[lane] != next_normal[lane])
                    changed = 1;
                vertex->pos[lane] = next_pos[lane];
                vertex->normal[lane] = next_normal[lane];
            }
            if (mesh->positions64) {
                for (int lane = 0; lane < 3; ++lane) {
                    double next = cloth3d_coord_or(pos[lane], 0.0);
                    if (mesh->positions64[p * 3 + lane] != next)
                        changed = 1;
                    mesh->positions64[p * 3 + lane] = next;
                }
            }
        }
    }
    if (changed)
        rt_mesh3d_touch_geometry(mesh);
}

/// @brief Advance the cloth by dt: anchor sync, fixed substeps, output bindings.
/// @param obj Cloth3D handle to advance.
/// @param dt Positive finite elapsed time accumulated into fixed simulation substeps.
void rt_cloth3d_step(void *obj, double dt) {
    rt_cloth3d *cloth = cloth3d_checked(obj, "Cloth3D.Step: invalid cloth");
    double max_catchup;
    if (!cloth || !isfinite(dt) || dt <= 0.0)
        return;
    cloth3d_sync_anchor(cloth);
    int32_t steps = 0;
    max_catchup = cloth->substep_dt * (double)CLOTH3D_MAX_SUBSTEPS;
    if (dt >= max_catchup) {
        double remainder = fmod(dt, cloth->substep_dt);
        cloth->accumulator += remainder;
        if (cloth->accumulator >= cloth->substep_dt)
            cloth->accumulator = fmod(cloth->accumulator, cloth->substep_dt);
        for (; steps < CLOTH3D_MAX_SUBSTEPS; ++steps)
            cloth3d_substep(cloth);
    } else {
        cloth->accumulator += dt;
    }
    while (cloth->accumulator >= cloth->substep_dt && steps < CLOTH3D_MAX_SUBSTEPS) {
        cloth3d_substep(cloth);
        cloth->accumulator -= cloth->substep_dt;
        ++steps;
    }
    if (steps == CLOTH3D_MAX_SUBSTEPS && cloth->accumulator >= cloth->substep_dt)
        cloth->accumulator = fmod(cloth->accumulator, cloth->substep_dt);
    if (steps > 0) {
        cloth3d_write_bone_overrides(cloth);
        cloth3d_write_mesh(cloth);
    }
}

/*==========================================================================
 * World registration
 *=========================================================================*/

/// @brief Register a cloth to tick inside World3D.StepSimulation.
/// @param world_obj World3D handle that retains the registration.
/// @param cloth_obj Cloth3D handle to retain once; duplicate registrations are ignored.
void rt_game3d_world_add_cloth(void *world_obj, void *cloth_obj) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.World3D.AddCloth: invalid world");
    rt_cloth3d *cloth = cloth3d_checked(cloth_obj, "Game3D.World3D.AddCloth: invalid cloth");
    int32_t new_capacity;
    if (!world || !cloth)
        return;
    if (world->cloth_count < 0 || world->cloth_capacity < 0 ||
        world->cloth_count > world->cloth_capacity ||
        (world->cloth_capacity > 0 && !world->cloths)) {
        rt_trap("Game3D.World3D.AddCloth: corrupt cloth storage");
        return;
    }
    for (int32_t i = 0; i < world->cloth_count; ++i)
        if (world->cloths[i] == cloth_obj)
            return;
    if (world->cloth_count >= world->cloth_capacity) {
        if (world->cloth_count == INT32_MAX || !game3d_checked_capacity_i32(world->cloth_capacity,
                                                                            world->cloth_count + 1,
                                                                            8,
                                                                            sizeof(*world->cloths),
                                                                            &new_capacity)) {
            rt_trap("Game3D.World3D.AddCloth: capacity overflow");
            return;
        }
        void **grown = (void **)realloc(world->cloths, (size_t)new_capacity * sizeof(void *));
        if (!grown) {
            rt_trap("Game3D.World3D.AddCloth: allocation failed");
            return;
        }
        world->cloths = grown;
        world->cloth_capacity = new_capacity;
    }
    world->cloths[world->cloth_count] = NULL;
    game3d_assign_typed_ref(&world->cloths[world->cloth_count], cloth_obj, RT_G3D_CLOTH3D_CLASS_ID);
    world->cloth_count += 1;
}

/// @brief Unregister a world-ticked cloth.
/// @param world_obj World3D handle whose registration list is searched.
/// @param cloth_obj Exact Cloth3D handle to release and remove.
void rt_game3d_world_remove_cloth(void *world_obj, void *cloth_obj) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.World3D.RemoveCloth: invalid world");
    if (!world || !cloth_obj)
        return;
    if (world->cloth_count < 0 || world->cloth_capacity < 0 ||
        world->cloth_count > world->cloth_capacity || (world->cloth_capacity > 0 && !world->cloths))
        return;
    for (int32_t i = 0; i < world->cloth_count; ++i) {
        if (world->cloths[i] != cloth_obj)
            continue;
        game3d_release_typed_ref(&world->cloths[i], RT_G3D_CLOTH3D_CLASS_ID);
        for (int32_t j = i; j + 1 < world->cloth_count; ++j)
            world->cloths[j] = world->cloths[j + 1];
        world->cloth_count -= 1;
        world->cloths[world->cloth_count] = NULL;
        return;
    }
}

/// @brief Per-step world hook: advance every registered cloth.
/// @param world Live World3D payload containing retained cloth handles.
/// @param dt Simulation elapsed time forwarded to each valid registered cloth.
void game3d_cloth_tick(struct rt_game3d_world *world, double dt) {
    if (!world || world->cloth_count < 0 || world->cloth_capacity < 0 ||
        world->cloth_count > world->cloth_capacity || (world->cloth_capacity > 0 && !world->cloths))
        return;
    for (int32_t i = 0; i < world->cloth_count; ++i) {
        rt_cloth3d *cloth =
            (rt_cloth3d *)rt_g3d_checked_or_null(world->cloths[i], RT_G3D_CLOTH3D_CLASS_ID);
        if (cloth)
            rt_cloth3d_step(cloth, dt);
    }
}

#else
typedef int rt_cloth3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
