//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_particles3d.c
// Purpose: 3D particle system — emitter spawning, Euler integration physics,
//   lifetime/size/color interpolation, camera-facing billboard quads,
//   and blend-mode-aware billboard rendering.
//
// Key invariants:
//   - Dead particles are swapped to end (unstable removal, O(1) per kill).
//   - Billboard quads use camera right/up vectors from the view matrix.
//   - Hardware draws upload compact center/axis/color instances against a retained unit quad;
//     software draws and trail ribbons fill reusable CPU vertex+index slots.
//   - Additive mode submits one batched draw; alpha mode sorts the batched quads
//     back-to-front before upload so transparent draw order is deterministic.
//   - xorshift32 PRNG seeded from a local monotonic counter for deterministic
//     randomization without object-address-derived seeds (no stdlib rand).
//   - Draw materials are slotted per frame so queued commands are not mutated by later draws.
//
// Ownership/Lifetime:
//   - Particles3D is GC-managed; finalizer frees the particle pool and drops
//     refs on texture and cached material.
//   - Overflow draw slots fall back to canvas-owned temp buffers freed at end-of-frame.
//
// Links: rt_particles3d.h, rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_particles3d.c
 * @brief Implements deterministic 3D particle emission, simulation, and billboard rendering.
 *
 * Particle state is advanced with bounded fixed time steps, while rendering supports sorted
 * alpha billboards, additive billboards, velocity stretching, and CPU-expanded ribbon trails.
 * Runtime handles and retained graphics resources are validated defensively at API boundaries.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_particles3d.h"
#include "rt_canvas3d.h"
#include "rt_vec3.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define PARTICLES3D_WORLD_ABS_MAX 1000000000000.0
#define PARTICLES3D_PARAM_MAX 1000000.0
#define PARTICLES3D_DRAW_SLOT_COUNT 4
#define PARTICLES3D_SIMULATION_STEP (1.0 / 60.0)
#define PARTICLES3D_MAX_SUBSTEPS 60

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_trap.h"
extern int rt_canvas3d_add_temp_buffer(void *canvas, void *buffer);

/*==========================================================================
 * Internal types
 *=========================================================================*/

typedef struct {
    double pos[3];
    double vel[3];
    float color[4]; /* current RGBA */
    float size;
    float life;
    float max_life;
    /* Authoritative simulation lifetime. The float fields remain suitable for render
     * interpolation, while this value prevents per-substep float rounding from moving an
     * expired particle past its exact endpoint. */
    double remaining_life;
} vgfx3d_particle_t;

typedef struct {
    void *vptr;
    vgfx3d_particle_t *particles;
    int32_t count;
    int32_t max_particles;

    double position[3];
    double emit_dir[3];
    double emit_spread;
    double speed_min, speed_max;
    double life_min, life_max;
    double size_start, size_end;
    double gravity[3];
    float color_start[3], color_end[3]; /* RGB [0,1] */
    double alpha_start, alpha_end;
    double rate;
    double accumulator;

    int8_t emitting;
    int8_t additive_blend;
    /* Velocity-aligned stretching: 0 = camera-facing quads, k scales length
     * by (1 + k * |velocity|). */
    double stretch_k;
    /* Ribbon trails: per-particle ring of recent positions (parallel arrays,
     * swapped alongside the particle pool's swap-remove kills). */
    float trail_lifetime;   /* seconds of retained history (0 = trails off) */
    int32_t trail_segments; /* control points per particle (2..16) */
    float *trail_pos;       /* max_particles * trail_segments * 3 */
    float *trail_age;       /* seconds since last control-point push */
    int16_t *trail_len;     /* control points currently stored */
    int16_t *trail_head;    /* ring head index */
    double softness;        /* Plan 10: soft-particle fade distance in world units (0 = off) */
    void *texture;
    int32_t emitter_shape; /* 0=point, 1=sphere, 2=box */
    double emitter_size[3];
    uint32_t prng_state;   /* per-instance PRNG seed */
    void *cached_material; /* reused across frames (GFX-052) */
    vgfx3d_vertex_t *draw_vertices[PARTICLES3D_DRAW_SLOT_COUNT];
    uint32_t *draw_indices[PARTICLES3D_DRAW_SLOT_COUNT];
    uint32_t draw_vertex_capacity[PARTICLES3D_DRAW_SLOT_COUNT];
    uint32_t draw_index_capacity[PARTICLES3D_DRAW_SLOT_COUNT];
    void *draw_materials[PARTICLES3D_DRAW_SLOT_COUNT];
    vgfx3d_vertex_t **overflow_draw_vertices;
    uint32_t **overflow_draw_indices;
    uint32_t *overflow_draw_vertex_capacity;
    uint32_t *overflow_draw_index_capacity;
    void **overflow_draw_materials;
    int32_t overflow_draw_slot_capacity;
    void *sort_keys;
    void *sort_scratch;
    int32_t sort_key_capacity;
    uint64_t sort_key_grow_count;
    int64_t draw_frame_serial;
    int32_t draw_slots_used;
    /* Fixed-step, terminal-frame, and compact-render state are appended to the private payload so
     * existing implementation-only prefix views remain valid. Terminal snapshots occupy the pool
     * tail and are excluded from `count`; live capacity plus terminal_count never exceeds
     * max_particles. */
    double simulation_residual;
    double dropped_time_total;
    double dropped_time_last_update;
    int32_t terminal_count;
    int8_t render_final_frame;
    /* Compact hardware records are emitter-owned scratch only. Canvas3D snapshots the live prefix
     * into frame storage before Draw returns, so repeated frames reuse this allocation safely. */
    vgfx3d_particle_instance_t *instance_scratch;
    int32_t instance_scratch_capacity;
    uint64_t instance_scratch_grow_count;
} rt_particles3d;

/// @brief Generate a non-zero per-instance seed for Particles3D.
/// @details Uses a process-local monotonic counter mixed with an odd constant
///          instead of deriving seeds from object addresses. The helper is
///          self-contained so graphics contract tests that compile this source
///          without the full runtime RNG still link cleanly.
/// @return Non-zero xorshift32 seed.
static uint32_t particles3d_next_seed(void) {
    static int64_t counter = INT64_C(0xA341316C);
    int64_t old = rt_atomic_fetch_add_i64(&counter, INT64_C(0x9E3779B9), __ATOMIC_RELAXED);
    uint32_t seed = (uint32_t)old ^ 0x12345678u;
    return seed ? seed : 0xA341316Cu;
}

/// @brief Validate @p obj as a Particles3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Candidate runtime object handle.
/// @return Typed particle-system pointer, or NULL when @p obj is invalid or has the wrong class.
static rt_particles3d *particles3d_checked(void *obj) {
    return (rt_particles3d *)rt_g3d_checked_or_null(obj, RT_G3D_PARTICLES3D_CLASS_ID);
}

/// @brief Drop one retained object ref and clear the slot.
/// @param slot Address of the retained-reference slot to release; NULL is accepted.
static void particles3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Return true when @p texture is a live Pixels object.
/// @param texture Candidate Pixels runtime object.
/// @return Nonzero when @p texture is a valid live Pixels object, otherwise zero.
static int particles3d_texture_valid(void *texture) {
    return rt_pixels_checked_impl_or_null(texture) != NULL;
}

/// @brief Release a retained Pixels slot only if it still points at Pixels.
/// @param slot Address of the texture slot; invalid unowned contents are cleared without release.
static void particles3d_release_texture_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!particles3d_texture_valid(*slot)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    particles3d_release_ref(slot);
}

/// @brief Release a retained Material3D slot only if it still points at Material3D.
/// @param slot Address of the material slot; invalid unowned contents are cleared without release.
static void particles3d_release_material_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MATERIAL3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    particles3d_release_ref(slot);
}

/// @brief Retain-then-release assignment for the particle texture slot.
/// @param slot Address of the retained texture slot to update.
/// @param value New Pixels handle, or NULL to clear the slot.
static void particles3d_assign_texture_ref(void **slot, void *value) {
    if (!slot || *slot == value)
        return;
    rt_obj_retain_maybe(value);
    particles3d_release_texture_slot(slot);
    *slot = value;
}

/// @brief Clear corrupted retained refs before update/draw/finalize paths use them.
/// @param ps Particle system whose texture and material slots are repaired.
static void particles3d_repair_refs(rt_particles3d *ps) {
    if (!ps)
        return;
    if (ps->texture && !particles3d_texture_valid(ps->texture))
        particles3d_release_texture_slot(&ps->texture);
    if (ps->cached_material && !rt_g3d_has_class(ps->cached_material, RT_G3D_MATERIAL3D_CLASS_ID))
        particles3d_release_material_slot(&ps->cached_material);
    for (int i = 0; i < PARTICLES3D_DRAW_SLOT_COUNT; ++i) {
        if (ps->draw_materials[i] &&
            !rt_g3d_has_class(ps->draw_materials[i], RT_G3D_MATERIAL3D_CLASS_ID))
            particles3d_release_material_slot(&ps->draw_materials[i]);
    }
    for (int32_t i = 0; i < ps->overflow_draw_slot_capacity; ++i) {
        if (ps->overflow_draw_materials && ps->overflow_draw_materials[i] &&
            !rt_g3d_has_class(ps->overflow_draw_materials[i], RT_G3D_MATERIAL3D_CLASS_ID))
            particles3d_release_material_slot(&ps->overflow_draw_materials[i]);
    }
}

/*==========================================================================
 * xorshift32 PRNG (per-instance, deterministic)
 *=========================================================================*/

/// @brief George Marsaglia's xorshift32 PRNG stepped in the emitter's per-instance
/// state. Deterministic for a given seed so two runs of the same emitter replay the
/// same spawn distribution — useful for reproducible recordings and unit tests.
/// Period is 2^32 - 1; fast enough to invoke many times per particle-spawn.
/// @param ps Particle system whose nonzero PRNG state is advanced.
/// @return Next deterministic 32-bit pseudorandom value.
static uint32_t xorshift32(rt_particles3d *ps) {
    if (!ps)
        return 0xA341316Cu;
    if (ps->prng_state == 0)
        ps->prng_state = 0xA341316Cu;
    uint32_t x = ps->prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    ps->prng_state = x;
    return x;
}

/// @brief Random float in [0, 1).
/// @param ps Particle system supplying the PRNG state.
/// @return Uniform pseudorandom value in the half-open interval `[0, 1)`.
static float randf(rt_particles3d *ps) {
    return (float)(xorshift32(ps) & 0x00FFFFFF) / (float)0x01000000;
}

/// @brief Random float in [lo, hi].
/// @param ps Particle system supplying the PRNG state.
/// @param lo Requested lower bound; invalid and negative values are sanitized.
/// @param hi Requested upper bound; invalid and negative values are sanitized.
/// @return Uniform pseudorandom value between the sanitized, ordered bounds.
static float rand_range(rt_particles3d *ps, double lo, double hi) {
    if (!isfinite(lo) || lo < 0.0)
        lo = 0.0;
    if (!isfinite(hi) || hi < 0.0)
        hi = 0.0;
    if (lo > PARTICLES3D_PARAM_MAX)
        lo = PARTICLES3D_PARAM_MAX;
    if (hi > PARTICLES3D_PARAM_MAX)
        hi = PARTICLES3D_PARAM_MAX;
    if (hi < lo) {
        double tmp = lo;
        lo = hi;
        hi = tmp;
    }
    return (float)(lo + randf(ps) * (hi - lo));
}

/*==========================================================================
 * Helpers
 *=========================================================================*/

/// @brief Split a 24-bit packed sRGB colour (0xRRGGBB layout) into three float channels
/// normalized to `[0, 1]`. Used to translate user-facing `int64` colour params into the
/// float vector format the mixer/renderer expects. Alpha is not part of the packed
/// format — callers manage alpha separately.
/// @param packed Packed `0xRRGGBB` color value.
/// @param rgb Three-element output array receiving normalized red, green, and blue channels.
static void unpack_color(int64_t packed, float *rgb) {
    rgb[0] = (float)((packed >> 16) & 0xFF) / 255.0f;
    rgb[1] = (float)((packed >> 8) & 0xFF) / 255.0f;
    rgb[2] = (float)(packed & 0xFF) / 255.0f;
}

/// @brief Return `value` if it is finite, otherwise return `fallback`.
/// @details Used by all particle setters for position, gravity, and direction
///   components to silently absorb NaN/Inf from user code without trapping.
/// @param value Candidate floating-point value.
/// @param fallback Replacement used when @p value is not finite.
/// @return @p value when finite, otherwise @p fallback.
static double particles_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Clamp `value` into `[-max_abs, max_abs]`, substituting `fallback` when not finite.
/// @param value Candidate floating-point value.
/// @param fallback Replacement used when @p value is not finite.
/// @param max_abs Inclusive absolute-value limit.
/// @return Finite value constrained to the requested symmetric interval.
static double particles_clamp_abs_or(double value, double fallback, double max_abs) {
    value = particles_finite_or(value, fallback);
    if (value > max_abs)
        return max_abs;
    if (value < -max_abs)
        return -max_abs;
    return value;
}

/// @brief Clamp a parameter to [0, +inf), converting NaN/Inf and negatives to 0.
/// @details Applied to speeds, rates, sizes, and lifetimes that have no meaningful
///   negative value — negative inputs are treated as zero rather than as an error
///   so callers can pass unchecked user-supplied values.
/// @param value Candidate nonnegative particle parameter.
/// @return @p value clamped to the supported nonnegative parameter range, or zero if invalid.
static double particles_nonnegative_or_zero(double value) {
    if (!isfinite(value) || value < 0.0)
        return 0.0;
    if (value > PARTICLES3D_PARAM_MAX)
        return PARTICLES3D_PARAM_MAX;
    return value;
}

/// @brief Clamp `value` to [lo, hi], converting NaN/Inf to `lo`.
/// @details Used for bounded parameters like spread and alpha where the
///   valid range is bounded on both ends and NaN must be handled gracefully.
/// @param value Candidate value to constrain.
/// @param lo Inclusive lower bound and fallback for non-finite input.
/// @param hi Inclusive upper bound.
/// @return Value constrained to `[`@p lo`, `@p hi`]`.
static double particles_clamp(double value, double lo, double hi) {
    if (!isfinite(value))
        return lo;
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

/// @brief Normalize a mutable 3-vector in place. NULL-safe and epsilon-guarded — vectors
/// with length below 1e-8f are left untouched so accumulated "tiny but non-zero" drift
/// doesn't produce huge normalized outputs. Used by spawn-direction computation on cone,
/// sphere, and disc emitters.
/// @param x Address of the vector's x component.
/// @param y Address of the vector's y component.
/// @param z Address of the vector's z component.
static void normalize3(float *x, float *y, float *z) {
    float max_component;
    if (!x || !y || !z)
        return;
    *x = (float)particles_clamp_abs_or((double)*x, 0.0, PARTICLES3D_PARAM_MAX);
    *y = (float)particles_clamp_abs_or((double)*y, 0.0, PARTICLES3D_PARAM_MAX);
    *z = (float)particles_clamp_abs_or((double)*z, 0.0, PARTICLES3D_PARAM_MAX);
    max_component = fmaxf(fabsf(*x), fmaxf(fabsf(*y), fabsf(*z)));
    if (!isfinite(max_component) || max_component <= 1e-8f)
        return;
    float sx = *x / max_component;
    float sy = *y / max_component;
    float sz = *z / max_component;
    float len = sqrtf(sx * sx + sy * sy + sz * sz);
    if (!isfinite(len) || len <= 1e-8f)
        return;
    *x = sx / len;
    *y = sy / len;
    *z = sz / len;
}

/// @brief Generate a random direction within a cone of half-angle `spread`
///        around the given direction vector.
/// @param ps Particle system supplying the deterministic PRNG state.
/// @param dir Three-component cone axis.
/// @param spread Cone half-angle in radians, clamped to `[0, pi]`.
/// @param out Three-element output array receiving a normalized direction.
static void random_cone_dir(rt_particles3d *ps, const double *dir, double spread, float *out) {
    if (!ps || !dir || !out)
        return;
    spread = particles_clamp(spread, 0.0, M_PI);
    if (spread <= 0.0) {
        out[0] = (float)dir[0];
        out[1] = (float)dir[1];
        out[2] = (float)dir[2];
        normalize3(&out[0], &out[1], &out[2]);
        if (fabsf(out[0]) + fabsf(out[1]) + fabsf(out[2]) <= 1e-6f) {
            out[0] = 0.0f;
            out[1] = 1.0f;
            out[2] = 0.0f;
        }
        return;
    }

    /* Random angle within cone */
    float cos_theta = 1.0f - randf(ps) * (1.0f - cosf((float)spread));
    cos_theta = (float)particles_clamp((double)cos_theta, -1.0, 1.0);
    float theta = acosf(cos_theta);
    float phi = randf(ps) * (float)(2.0 * M_PI);

    /* Build a coordinate frame around dir */
    float d[3] = {(float)particles_clamp_abs_or(dir[0], 0.0, PARTICLES3D_PARAM_MAX),
                  (float)particles_clamp_abs_or(dir[1], 1.0, PARTICLES3D_PARAM_MAX),
                  (float)particles_clamp_abs_or(dir[2], 0.0, PARTICLES3D_PARAM_MAX)};
    normalize3(&d[0], &d[1], &d[2]);
    if (fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]) <= 1e-6f) {
        d[0] = 0;
        d[1] = 1;
        d[2] = 0;
    }

    /* Find a perpendicular vector */
    float perp[3];
    if (fabsf(d[1]) < 0.9f) {
        perp[0] = 0;
        perp[1] = 1;
        perp[2] = 0;
    } else {
        perp[0] = 1;
        perp[1] = 0;
        perp[2] = 0;
    }
    /* right = cross(d, perp), normalized */
    float rx = d[1] * perp[2] - d[2] * perp[1];
    float ry = d[2] * perp[0] - d[0] * perp[2];
    float rz = d[0] * perp[1] - d[1] * perp[0];
    normalize3(&rx, &ry, &rz);
    if (fabsf(rx) + fabsf(ry) + fabsf(rz) <= 1e-6f) {
        rx = 1.0f;
        ry = 0.0f;
        rz = 0.0f;
    }

    /* up = cross(right, d) */
    float ux = ry * d[2] - rz * d[1];
    float uy = rz * d[0] - rx * d[2];
    float uz = rx * d[1] - ry * d[0];

    float st = sinf(theta), ct = cosf(theta);
    float sp = sinf(phi), cp = cosf(phi);

    out[0] = d[0] * ct + rx * st * cp + ux * st * sp;
    out[1] = d[1] * ct + ry * st * cp + uy * st * sp;
    out[2] = d[2] * ct + rz * st * cp + uz * st * sp;

    normalize3(&out[0], &out[1], &out[2]);
    if (fabsf(out[0]) + fabsf(out[1]) + fabsf(out[2]) <= 1e-6f) {
        out[0] = d[0];
        out[1] = d[1];
        out[2] = d[2];
    }
}

/// @brief True if a particle's position, velocity, color, alpha and (non-negative)
///        size are all finite — used to drop corrupted particles before upload.
/// @param p Particle state to validate.
/// @return Nonzero when every simulation and render field is finite and within bounds.
static int particle_state_is_finite(const vgfx3d_particle_t *p) {
    if (!p)
        return 0;
    for (int i = 0; i < 3; i++) {
        if (!isfinite(p->pos[i]) || !isfinite(p->vel[i]) || !isfinite(p->color[i]))
            return 0;
        if (fabs(p->pos[i]) > PARTICLES3D_WORLD_ABS_MAX || fabs(p->vel[i]) > PARTICLES3D_PARAM_MAX)
            return 0;
        if (p->color[i] < 0.0f || p->color[i] > 1.0f)
            return 0;
    }
    return isfinite(p->color[3]) && p->color[3] >= 0.0f && p->color[3] <= 1.0f &&
           isfinite(p->size) && p->size >= 0.0f && p->size <= (float)PARTICLES3D_PARAM_MAX &&
           isfinite(p->life) && isfinite(p->max_life) && p->max_life > 0.0f &&
           isfinite(p->remaining_life) && p->remaining_life >= 0.0 &&
           p->remaining_life <= (double)p->max_life;
}

/*==========================================================================
 * Lifecycle
 *=========================================================================*/

/// @brief GC finalizer for Particles3D. Frees the particle pool, drops the texture
/// reference (if any), and drops the per-emitter cached material (built once and reused
/// across frames per GFX-052). Each release-check is independent so a missing texture
/// doesn't prevent material teardown or vice versa.
/// @param obj Particles3D runtime object being finalized; NULL is accepted.
static void rt_particles3d_finalize(void *obj) {
    rt_particles3d *ps = (rt_particles3d *)obj;
    if (!ps)
        return;
    free(ps->particles);
    ps->particles = NULL;
    free(ps->trail_pos);
    ps->trail_pos = NULL;
    free(ps->trail_age);
    ps->trail_age = NULL;
    free(ps->trail_len);
    ps->trail_len = NULL;
    free(ps->trail_head);
    ps->trail_head = NULL;
    for (int i = 0; i < PARTICLES3D_DRAW_SLOT_COUNT; ++i) {
        free(ps->draw_vertices[i]);
        ps->draw_vertices[i] = NULL;
        free(ps->draw_indices[i]);
        ps->draw_indices[i] = NULL;
        particles3d_release_material_slot(&ps->draw_materials[i]);
    }
    for (int32_t i = 0; i < ps->overflow_draw_slot_capacity; ++i) {
        if (ps->overflow_draw_vertices)
            free(ps->overflow_draw_vertices[i]);
        if (ps->overflow_draw_indices)
            free(ps->overflow_draw_indices[i]);
        if (ps->overflow_draw_materials)
            particles3d_release_material_slot(&ps->overflow_draw_materials[i]);
    }
    free(ps->overflow_draw_vertices);
    free(ps->overflow_draw_indices);
    free(ps->overflow_draw_vertex_capacity);
    free(ps->overflow_draw_index_capacity);
    free(ps->overflow_draw_materials);
    ps->overflow_draw_vertices = NULL;
    ps->overflow_draw_indices = NULL;
    ps->overflow_draw_vertex_capacity = NULL;
    ps->overflow_draw_index_capacity = NULL;
    ps->overflow_draw_materials = NULL;
    ps->overflow_draw_slot_capacity = 0;
    free(ps->sort_keys);
    ps->sort_keys = NULL;
    free(ps->sort_scratch);
    ps->sort_scratch = NULL;
    ps->sort_key_capacity = 0;
    ps->sort_key_grow_count = 0;
    free(ps->instance_scratch);
    ps->instance_scratch = NULL;
    ps->instance_scratch_capacity = 0;
    ps->instance_scratch_grow_count = 0;
    particles3d_release_texture_slot(&ps->texture);
    particles3d_release_material_slot(&ps->cached_material);
}

/// @brief Allocate a new particle emitter with an internally-sized pool of up to
/// `max_particles` concurrent particles. Rejects 0 / negative / >100000 to keep a predictable
/// memory ceiling — the pool is calloc'd up front so spawn/kill cost stays O(1) with no
/// re-allocation. Defaults are tuned for a generic sparkle: upward cone emit, ~17° spread,
/// 1-3 u/s speed, 0.5-1.5 s lifetime, 0.2 → 0.05 size taper, 9.8 u/s² gravity, white colour,
/// 1.0 → 0.0 alpha fade, rate 20 /s, alpha-blend mode, point-source emitter. Each emitter receives
/// a nonzero seed from a process-local monotonic sequence; callers can override it for reproducible
/// spawn distributions.
/// @param max_particles Maximum number of concurrent live particles, from 1 through 100000.
/// @return New GC-managed Particles3D handle, or NULL after trapping on invalid input or allocation
/// failure.
void *rt_particles3d_new(int64_t max_particles) {
    if (max_particles <= 0 || max_particles > 100000) {
        rt_trap("Particles3D.New: max_particles must be 1-100000");
        return NULL;
    }
    rt_particles3d *ps = (rt_particles3d *)rt_obj_new_i64(RT_G3D_PARTICLES3D_CLASS_ID,
                                                          (int64_t)sizeof(rt_particles3d));
    if (!ps) {
        rt_trap("Particles3D.New: memory allocation failed");
        return NULL;
    }

    ps->vptr = NULL;
    ps->particles = (vgfx3d_particle_t *)calloc((size_t)max_particles, sizeof(vgfx3d_particle_t));
    if (!ps->particles) {
        if (rt_obj_release_check0(ps))
            rt_obj_free(ps);
        rt_trap("Particles3D.New: out of memory");
        return NULL;
    }
    ps->count = 0;
    ps->max_particles = (int32_t)max_particles;

    ps->position[0] = ps->position[1] = ps->position[2] = 0.0;
    ps->emit_dir[0] = 0.0;
    ps->emit_dir[1] = 1.0;
    ps->emit_dir[2] = 0.0;
    ps->emit_spread = 0.3;
    ps->speed_min = 1.0;
    ps->speed_max = 3.0;
    ps->life_min = 0.5;
    ps->life_max = 1.5;
    ps->size_start = 0.2;
    ps->size_end = 0.05;
    ps->gravity[0] = 0;
    ps->gravity[1] = -9.8;
    ps->gravity[2] = 0;
    ps->color_start[0] = 1;
    ps->color_start[1] = 1;
    ps->color_start[2] = 1;
    ps->color_end[0] = 1;
    ps->color_end[1] = 1;
    ps->color_end[2] = 1;
    ps->alpha_start = 1.0;
    ps->alpha_end = 0.0;
    ps->rate = 20.0;
    ps->accumulator = 0.0;
    ps->emitting = 0;
    ps->additive_blend = 0;
    ps->stretch_k = 0.0;
    ps->trail_lifetime = 0.0f;
    ps->trail_segments = 0;
    ps->trail_pos = NULL;
    ps->trail_age = NULL;
    ps->trail_len = NULL;
    ps->trail_head = NULL;
    ps->softness = 0.0;
    ps->texture = NULL;
    ps->emitter_shape = 0;
    ps->emitter_size[0] = ps->emitter_size[1] = ps->emitter_size[2] = 1.0;
    ps->prng_state = particles3d_next_seed();
    ps->cached_material = NULL;
    for (int i = 0; i < PARTICLES3D_DRAW_SLOT_COUNT; ++i) {
        ps->draw_vertices[i] = NULL;
        ps->draw_indices[i] = NULL;
        ps->draw_vertex_capacity[i] = 0;
        ps->draw_index_capacity[i] = 0;
        ps->draw_materials[i] = NULL;
    }
    ps->overflow_draw_vertices = NULL;
    ps->overflow_draw_indices = NULL;
    ps->overflow_draw_vertex_capacity = NULL;
    ps->overflow_draw_index_capacity = NULL;
    ps->overflow_draw_materials = NULL;
    ps->overflow_draw_slot_capacity = 0;
    ps->sort_keys = NULL;
    ps->sort_scratch = NULL;
    ps->sort_key_capacity = 0;
    ps->sort_key_grow_count = 0;
    ps->draw_frame_serial = -1;
    ps->draw_slots_used = 0;
    ps->simulation_residual = 0.0;
    ps->dropped_time_total = 0.0;
    ps->dropped_time_last_update = 0.0;
    ps->terminal_count = 0;
    ps->render_final_frame = 1;
    ps->instance_scratch = NULL;
    ps->instance_scratch_capacity = 0;
    ps->instance_scratch_grow_count = 0;

    rt_obj_set_finalizer(ps, rt_particles3d_finalize);
    return ps;
}

/*==========================================================================
 * Configuration
 *=========================================================================*/

/// @brief Set the emitter origin in world space. New particles spawn at this point (offset by
/// the emitter shape if non-point).
/// @param o Particles3D handle; invalid handles are ignored.
/// @param x World-space x coordinate, clamped to the supported world range.
/// @param y World-space y coordinate, clamped to the supported world range.
/// @param z World-space z coordinate, clamped to the supported world range.
void rt_particles3d_set_position(void *o, double x, double y, double z) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    p->position[0] = particles_clamp_abs_or(x, 0.0, PARTICLES3D_WORLD_ABS_MAX);
    p->position[1] = particles_clamp_abs_or(y, 0.0, PARTICLES3D_WORLD_ABS_MAX);
    p->position[2] = particles_clamp_abs_or(z, 0.0, PARTICLES3D_WORLD_ABS_MAX);
}

/// @brief Set the average emit direction (normalized internally) and cone half-angle in radians.
/// spread=0 means perfectly aligned, spread=PI means full sphere.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param dx Emit-axis x component.
/// @param dy Emit-axis y component.
/// @param dz Emit-axis z component.
/// @param spread Cone half-angle in radians, or degrees when greater than pi and at most 180.
void rt_particles3d_set_direction(void *o, double dx, double dy, double dz, double spread) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    double dir[3] = {
        particles_clamp_abs_or(dx, 0.0, PARTICLES3D_PARAM_MAX),
        particles_clamp_abs_or(dy, 1.0, PARTICLES3D_PARAM_MAX),
        particles_clamp_abs_or(dz, 0.0, PARTICLES3D_PARAM_MAX),
    };
    double max_component = fmax(fabs(dir[0]), fmax(fabs(dir[1]), fabs(dir[2])));
    int ok = 0;
    if (isfinite(max_component) && max_component > 1e-8) {
        double sx = dir[0] / max_component;
        double sy = dir[1] / max_component;
        double sz = dir[2] / max_component;
        double len = sqrt(sx * sx + sy * sy + sz * sz);
        if (isfinite(len) && len > 1e-8) {
            p->emit_dir[0] = sx / len;
            p->emit_dir[1] = sy / len;
            p->emit_dir[2] = sz / len;
            ok = 1;
        }
    }
    if (!ok) {
        p->emit_dir[0] = 0.0;
        p->emit_dir[1] = 1.0;
        p->emit_dir[2] = 0.0;
    }
    spread = particles_finite_or(spread, 0.0);
    if (spread > M_PI && spread <= 180.0)
        spread *= (M_PI / 180.0);
    p->emit_spread = particles_clamp(spread, 0.0, M_PI);
}

/// @brief Set the per-particle initial speed range [mn, mx] in world-units/sec (uniform random).
/// @param o Particles3D handle; invalid handles are ignored.
/// @param mn First speed bound; negative or non-finite input becomes zero.
/// @param mx Second speed bound; negative or non-finite input becomes zero.
void rt_particles3d_set_speed(void *o, double mn, double mx) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    mn = particles_nonnegative_or_zero(mn);
    mx = particles_nonnegative_or_zero(mx);
    if (mx < mn) {
        double tmp = mn;
        mn = mx;
        mx = tmp;
    }
    if (mn > PARTICLES3D_PARAM_MAX)
        mn = PARTICLES3D_PARAM_MAX;
    if (mx > PARTICLES3D_PARAM_MAX)
        mx = PARTICLES3D_PARAM_MAX;
    p->speed_min = mn;
    p->speed_max = mx;
}

/// @brief Set the per-particle lifetime range [mn, mx] in seconds (uniform random per spawn).
/// @param o Particles3D handle; invalid handles are ignored.
/// @param mn First lifetime bound, clamped to the supported positive range.
/// @param mx Second lifetime bound, clamped to the supported positive range.
void rt_particles3d_set_lifetime(void *o, double mn, double mx) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    mn = particles_finite_or(mn, 0.01);
    mx = particles_finite_or(mx, mn);
    if (mn < 0.01)
        mn = 0.01;
    if (mx < 0.01)
        mx = 0.01;
    if (mx < mn) {
        double tmp = mn;
        mn = mx;
        mx = tmp;
    }
    if (mn > PARTICLES3D_PARAM_MAX)
        mn = PARTICLES3D_PARAM_MAX;
    if (mx > PARTICLES3D_PARAM_MAX)
        mx = PARTICLES3D_PARAM_MAX;
    p->life_min = mn;
    p->life_max = mx;
}

/// @brief Set the start and end size (interpolated by age) for each particle.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param s Size at spawn, sanitized to the supported nonnegative range.
/// @param e Size at expiration, sanitized to the supported nonnegative range.
void rt_particles3d_set_size(void *o, double s, double e) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    p->size_start = particles_nonnegative_or_zero(s);
    p->size_end = particles_nonnegative_or_zero(e);
}

/// @brief Set the constant acceleration applied to every particle each frame (typical: (0,-9.8,0)).
/// @param o Particles3D handle; invalid handles are ignored.
/// @param gx World-space x acceleration.
/// @param gy World-space y acceleration.
/// @param gz World-space z acceleration.
void rt_particles3d_set_gravity(void *o, double gx, double gy, double gz) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    p->gravity[0] = particles_clamp_abs_or(gx, 0.0, PARTICLES3D_PARAM_MAX);
    p->gravity[1] = particles_clamp_abs_or(gy, 0.0, PARTICLES3D_PARAM_MAX);
    p->gravity[2] = particles_clamp_abs_or(gz, 0.0, PARTICLES3D_PARAM_MAX);
}

/// @brief Set start (`sc`) and end (`ec`) colors as packed 0xRRGGBB. Each particle linearly
/// interpolates between them based on age ratio. Alpha component is set separately via
/// `_set_alpha`.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param sc Packed `0xRRGGBB` color at spawn.
/// @param ec Packed `0xRRGGBB` color at expiration.
void rt_particles3d_set_color(void *o, int64_t sc, int64_t ec) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    unpack_color(sc, p->color_start);
    unpack_color(ec, p->color_end);
}

/// @brief Set start (`sa`) and end (`ea`) alpha values [0, 1]. Common pattern: 1.0→0.0 for
/// fade-out.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param sa Alpha at spawn, clamped to `[0, 1]`.
/// @param ea Alpha at expiration, clamped to `[0, 1]`.
void rt_particles3d_set_alpha(void *o, double sa, double ea) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    p->alpha_start = particles_clamp(sa, 0.0, 1.0);
    p->alpha_end = particles_clamp(ea, 0.0, 1.0);
}

/// @brief Set the spawn rate in particles per second. The accumulator pattern emits whole
/// particles when ≥1 worth has accumulated, preserving fractional rates across frames.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param r Requested particles per second, sanitized to the supported nonnegative range.
void rt_particles3d_set_rate(void *o, double r) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    p->rate = particles_nonnegative_or_zero(r);
}

/// @brief Set the soft-particle fade distance in world units (0 disables).
/// @details Particles fade out as their quads approach opaque geometry, hiding
///          the hard intersection line. Requires a backend with an opaque depth
///          snapshot (BackendSupports("soft-particles")); ignored elsewhere.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param distance Fade distance in world units; non-finite or negative input disables softness.
void rt_particles3d_set_softness(void *o, double distance) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    if (!isfinite(distance) || distance < 0.0)
        distance = 0.0;
    if (distance > PARTICLES3D_PARAM_MAX)
        distance = PARTICLES3D_PARAM_MAX;
    p->softness = distance;
}

/// @brief Select additive or conventional alpha blending for particle billboards.
/// @details Additive blending is suitable for fire and glow and skips back-to-front sorting because
/// draw order does not affect the result. Alpha blending retains deterministic depth sorting.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param a Nonzero to enable additive blending, zero to use alpha blending.
void rt_particles3d_set_additive(void *o, int8_t a) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    p->additive_blend = a ? 1 : 0;
}

/// @brief Read whether additive blending is enabled (ADR 0227).
/// @param o Particles3D receiver.
/// @return Nonzero for additive blending, or 0 for invalid handles.
int8_t rt_particles3d_get_additive(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p && p->additive_blend ? 1 : 0;
}

/// @brief Velocity-aligned billboard stretching: 0 = camera-facing quads,
///   k scales the quad length by (1 + k * |velocity|). Clamped to [0, 8].
/// @param o Particles3D handle; invalid handles are ignored.
/// @param k Nonnegative velocity-to-length scale, clamped to `[0, 8]`.
void rt_particles3d_set_stretch(void *o, double k) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    if (!isfinite(k) || k < 0.0)
        k = 0.0;
    if (k > 8.0)
        k = 8.0;
    p->stretch_k = k;
}

/// @brief Enable per-particle ribbon trails: a ring of the last N positions is
///   emitted as a camera-facing strip that tapers and fades toward the tail.
///   @p lifetime_sec of history spread over @p segments control points (2..16);
///   lifetime <= 0 disables trails and frees the history storage.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param lifetime_sec Seconds of trail history to retain; nonpositive values disable trails.
/// @param segments Number of ring-buffer control points per particle, clamped to `[2, 16]`.
void rt_particles3d_set_trail(void *o, double lifetime_sec, int64_t segments) {
    rt_particles3d *ps = particles3d_checked(o);
    if (!ps)
        return;
    if (!isfinite(lifetime_sec) || lifetime_sec <= 0.0) {
        free(ps->trail_pos);
        free(ps->trail_age);
        free(ps->trail_len);
        free(ps->trail_head);
        ps->trail_pos = NULL;
        ps->trail_age = NULL;
        ps->trail_len = NULL;
        ps->trail_head = NULL;
        ps->trail_lifetime = 0.0f;
        ps->trail_segments = 0;
        return;
    }
    if (segments < 2)
        segments = 2;
    if (segments > 16)
        segments = 16;
    if (ps->max_particles <= 0)
        return;
    if (ps->trail_pos && ps->trail_segments == (int32_t)segments) {
        ps->trail_lifetime = (float)lifetime_sec;
        return;
    }
    free(ps->trail_pos);
    free(ps->trail_age);
    free(ps->trail_len);
    free(ps->trail_head);
    size_t n = (size_t)ps->max_particles;
    ps->trail_pos = (float *)calloc(n * (size_t)segments * 3u, sizeof(float));
    ps->trail_age = (float *)calloc(n, sizeof(float));
    ps->trail_len = (int16_t *)calloc(n, sizeof(int16_t));
    ps->trail_head = (int16_t *)calloc(n, sizeof(int16_t));
    if (!ps->trail_pos || !ps->trail_age || !ps->trail_len || !ps->trail_head) {
        free(ps->trail_pos);
        free(ps->trail_age);
        free(ps->trail_len);
        free(ps->trail_head);
        ps->trail_pos = NULL;
        ps->trail_age = NULL;
        ps->trail_len = NULL;
        ps->trail_head = NULL;
        ps->trail_lifetime = 0.0f;
        ps->trail_segments = 0;
        return;
    }
    ps->trail_lifetime = (float)lifetime_sec;
    ps->trail_segments = (int32_t)segments;
}

/// @brief Swap-remove particle @p i, keeping the trail slot arrays in sync with
///   the pool's unstable removal so ribbons never jump between particles.
/// @param ps Particle system containing the active pool and parallel trail arrays.
/// @param i Live-particle index to remove.
static void particles3d_swap_kill(rt_particles3d *ps, int32_t i) {
    int32_t last = --ps->count;
    ps->particles[i] = ps->particles[last];
    if (ps->trail_pos && ps->trail_segments > 0 && i != last) {
        size_t stride = (size_t)ps->trail_segments * 3u;
        memcpy(&ps->trail_pos[(size_t)i * stride],
               &ps->trail_pos[(size_t)last * stride],
               stride * sizeof(float));
        ps->trail_age[i] = ps->trail_age[last];
        ps->trail_len[i] = ps->trail_len[last];
        ps->trail_head[i] = ps->trail_head[last];
    }
}

/// @brief Resolve one terminal-snapshot ordinal to its pool-tail storage index.
/// @details Live particles occupy `[0, count)` while terminal snapshots occupy descending slots
///   beginning at `max_particles - 1`. The regions cannot overlap when private count invariants
///   hold.
/// @param ps Particle system owning the shared pool.
/// @param ordinal Zero-based terminal snapshot ordinal.
/// @return Valid pool index, or -1 for invalid/corrupt state.
static int32_t particles3d_terminal_slot(const rt_particles3d *ps, int32_t ordinal) {
    if (!ps || !ps->particles || ordinal < 0 || ordinal >= ps->terminal_count ||
        ps->terminal_count > ps->max_particles - ps->count)
        return -1;
    return ps->max_particles - 1 - ordinal;
}

/// @brief Retain an expired particle endpoint in the non-live pool tail for the next Draw.
/// @details The caller must remove the particle from the live prefix first. No allocation occurs;
///   snapshots are bounded by the slots released by expiration during the current update.
/// @param ps Particle system receiving the endpoint snapshot.
/// @param endpoint Fully integrated finite terminal state with life equal to zero.
static void particles3d_retain_terminal_particle(rt_particles3d *ps,
                                                 const vgfx3d_particle_t *endpoint) {
    if (!ps || !endpoint || !ps->render_final_frame || !particle_state_is_finite(endpoint) ||
        ps->terminal_count < 0 || ps->terminal_count >= ps->max_particles - ps->count)
        return;
    int32_t slot = ps->max_particles - 1 - ps->terminal_count;
    ps->particles[slot] = *endpoint;
    ps->terminal_count++;
}

/// @brief Resolve one billboard ordinal across the live prefix followed by terminal snapshots.
/// @param ps Particle system supplying both representations.
/// @param ordinal Draw-order source ordinal in `[0, count + terminal_count)`.
/// @return Borrowed particle pointer, or NULL for corrupt/out-of-range state.
static vgfx3d_particle_t *particles3d_draw_particle_at(rt_particles3d *ps, int32_t ordinal) {
    if (!ps || ordinal < 0)
        return NULL;
    if (ordinal < ps->count)
        return &ps->particles[ordinal];
    int32_t slot = particles3d_terminal_slot(ps, ordinal - ps->count);
    return slot >= 0 ? &ps->particles[slot] : NULL;
}

/// @brief Set the per-particle billboard texture. NULL produces solid color quads.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param tex Pixels handle to retain, or NULL to remove the texture.
void rt_particles3d_set_texture(void *o, void *tex) {
    rt_particles3d *ps = particles3d_checked(o);
    if (!ps)
        return;
    if (tex && !rt_pixels_checked_impl_or_null(tex))
        return;
    if (ps->texture == tex)
        return;
    particles3d_assign_texture_ref(&ps->texture, tex);
}

/// @brief Select the emitter volume: 0 = point (default), 1 = sphere (uniform interior),
/// 2 = box. Combined with `_set_emitter_size` to control the spawn region.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param s Shape identifier, clamped to point (0), sphere (1), or box (2).
void rt_particles3d_set_emitter_shape(void *o, int64_t s) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    if (s < 0)
        s = 0;
    if (s > 2)
        s = 2;
    p->emitter_shape = (int32_t)s;
}

/// @brief Set the emitter shape's extent. For sphere: only sx is used (radius); for box: full
/// half-extents per axis. Ignored for point emitter.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param sx Nonnegative sphere radius or box x half-extent.
/// @param sy Nonnegative box y half-extent.
/// @param sz Nonnegative box z half-extent.
void rt_particles3d_set_emitter_size(void *o, double sx, double sy, double sz) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    p->emitter_size[0] = fabs(particles_clamp_abs_or(sx, 0.0, PARTICLES3D_PARAM_MAX));
    p->emitter_size[1] = fabs(particles_clamp_abs_or(sy, 0.0, PARTICLES3D_PARAM_MAX));
    p->emitter_size[2] = fabs(particles_clamp_abs_or(sz, 0.0, PARTICLES3D_PARAM_MAX));
}

/* Emitter readback (ADR 0233) — read peers over the retained emitter state,
 * named for the setter parameters they mirror. */

/// @brief Repack a normalized RGB float triple to the packed 0xRRGGBB form.
/// @param rgb Borrowed normalized channel triple.
/// @return Packed color matching the `SetColor` input convention.
static int64_t particles3d_pack_color(const float *rgb) {
    int64_t r = (int64_t)(rgb[0] * 255.0f + 0.5f);
    int64_t g = (int64_t)(rgb[1] * 255.0f + 0.5f);
    int64_t b = (int64_t)(rgb[2] * 255.0f + 0.5f);
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    if (b > 255)
        b = 255;
    return (r << 16) | (g << 8) | b;
}

/// @brief `Particles3D.Rate` — retained emission rate. @param o Borrowed handle.
double rt_particles3d_get_rate(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->rate : 0.0;
}

/// @brief `Particles3D.LifetimeMin` — retained minimum lifetime. @param o Borrowed handle.
double rt_particles3d_get_lifetime_min(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->life_min : 0.0;
}

/// @brief `Particles3D.LifetimeMax` — retained maximum lifetime. @param o Borrowed handle.
double rt_particles3d_get_lifetime_max(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->life_max : 0.0;
}

/// @brief `Particles3D.SpeedMin` — retained minimum emit speed. @param o Borrowed handle.
double rt_particles3d_get_speed_min(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->speed_min : 0.0;
}

/// @brief `Particles3D.SpeedMax` — retained maximum emit speed. @param o Borrowed handle.
double rt_particles3d_get_speed_max(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->speed_max : 0.0;
}

/// @brief `Particles3D.SizeStart` — retained spawn size. @param o Borrowed handle.
double rt_particles3d_get_size_start(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->size_start : 0.0;
}

/// @brief `Particles3D.SizeEnd` — retained end-of-life size. @param o Borrowed handle.
double rt_particles3d_get_size_end(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->size_end : 0.0;
}

/// @brief `Particles3D.AlphaStart` — retained spawn alpha. @param o Borrowed handle.
double rt_particles3d_get_alpha_start(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->alpha_start : 0.0;
}

/// @brief `Particles3D.AlphaEnd` — retained end-of-life alpha. @param o Borrowed handle.
double rt_particles3d_get_alpha_end(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->alpha_end : 0.0;
}

/// @brief `Particles3D.ColorStart` — packed 0xRRGGBB spawn color. @param o Borrowed handle.
int64_t rt_particles3d_get_color_start(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? particles3d_pack_color(p->color_start) : 0;
}

/// @brief `Particles3D.ColorEnd` — packed 0xRRGGBB end color. @param o Borrowed handle.
int64_t rt_particles3d_get_color_end(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? particles3d_pack_color(p->color_end) : 0;
}

/// @brief `Particles3D.Gravity` — fresh Vec3 of the retained gravity. @param o Borrowed handle.
void *rt_particles3d_get_gravity(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(p->gravity[0], p->gravity[1], p->gravity[2]);
}

/// @brief `Particles3D.Position` — fresh Vec3 of the emitter origin. @param o Borrowed handle.
void *rt_particles3d_get_position_vec3(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(p->position[0], p->position[1], p->position[2]);
}

/// @brief `Particles3D.Direction` — fresh Vec3 of the emit direction. @param o Borrowed handle.
void *rt_particles3d_get_direction(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(p->emit_dir[0], p->emit_dir[1], p->emit_dir[2]);
}

/// @brief `Particles3D.Spread` — retained emit-cone spread. @param o Borrowed handle.
double rt_particles3d_get_spread(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->emit_spread : 0.0;
}

/// @brief `Particles3D.EmitterShape` — retained shape id (0=point, 1=sphere, 2=box).
/// @param o Borrowed handle.
int64_t rt_particles3d_get_emitter_shape(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? (int64_t)p->emitter_shape : 0;
}

/// @brief `Particles3D.EmitterSize` — fresh Vec3 of the emitter extents. @param o Borrowed handle.
void *rt_particles3d_get_emitter_size(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(p->emitter_size[0], p->emitter_size[1], p->emitter_size[2]);
}

/// @brief `Particles3D.Stretch` — retained velocity-stretch factor. @param o Borrowed handle.
double rt_particles3d_get_stretch(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->stretch_k : 0.0;
}

/// @brief `Particles3D.TrailLifetime` — retained trail history seconds. @param o Borrowed handle.
double rt_particles3d_get_trail_lifetime(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? (double)p->trail_lifetime : 0.0;
}

/// @brief `Particles3D.TrailSegments` — retained trail control points. @param o Borrowed handle.
int64_t rt_particles3d_get_trail_segments(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? (int64_t)p->trail_segments : 0;
}

/// @brief `Particles3D.Softness` — retained soft-particle fade distance. @param o Borrowed handle.
double rt_particles3d_get_softness(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? p->softness : 0.0;
}

/// @brief `Particles3D.Texture` — borrowed particle texture or NULL. @param o Borrowed handle.
void *rt_particles3d_get_texture(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p && particles3d_texture_valid(p->texture) ? p->texture : NULL;
}

/*==========================================================================
 * Playback
 *=========================================================================*/

/// @brief Begin emitting (continuous spawn at the configured rate). Existing live particles
/// continue to update regardless of the emit flag.
/// @param o Particles3D handle; invalid handles are ignored.
void rt_particles3d_start(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    if (p)
        p->emitting = 1;
}

/// @brief Stop continuous emission. Existing particles run to natural lifetime; for instant
/// removal use `_clear`.
/// @param o Particles3D handle; invalid handles are ignored.
void rt_particles3d_stop(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    if (p)
        p->emitting = 0;
}

/// @brief Kill every live particle and reset the spawn accumulator. Doesn't change emit state.
/// @param o Particles3D handle; invalid handles are ignored.
void rt_particles3d_clear(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    p->count = 0;
    p->accumulator = 0.0;
    p->simulation_residual = 0.0;
    p->terminal_count = 0;
}

/// @brief Number of particles currently alive.
/// @param o Particles3D handle.
/// @return Validated live-particle count, or zero for an invalid or corrupt handle.
int64_t rt_particles3d_get_count(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p || !p->particles || p->max_particles <= 0 || p->count <= 0)
        return 0;
    return p->count > p->max_particles ? p->max_particles : p->count;
}

/// @brief `Particles3D.set_Seed` — set the live spawn PRNG state.
/// @details Emitters default to a process-wide counter seed, so spawn streams
///   depend on global construction order; setting an explicit seed makes the
///   stream reproducible for determinism-sensitive content and replays. Zero
///   maps to the non-zero xorshift fallback constant.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param seed New 32-bit spawn seed; zero selects the built-in nonzero fallback.
void rt_particles3d_set_seed(void *o, int64_t seed) {
    rt_particles3d *p = particles3d_checked(o);
    if (!p)
        return;
    p->prng_state = (uint32_t)seed ? (uint32_t)seed : 0xA341316Cu;
}

/// @brief `Particles3D.get_Seed` — current spawn PRNG state (checkpointable).
/// @param o Particles3D handle.
/// @return Current unsigned 32-bit PRNG state widened to `int64_t`, or zero for an invalid handle.
int64_t rt_particles3d_get_seed(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p ? (int64_t)p->prng_state : 0;
}

/// @brief Copy the emitter world position into @p out. Used by floating-origin rebase tests to
///   verify the emitter shifted; zeroed if the handle is invalid or @p out is NULL.
/// @param o Particles3D handle.
/// @param out Three-element output array; no data is written when NULL.
void rt_particles3d_get_position(void *o, double out[3]) {
    rt_particles3d *p = particles3d_checked(o);
    if (!out)
        return;
    out[0] = p ? particles_clamp_abs_or(p->position[0], 0.0, PARTICLES3D_WORLD_ABS_MAX) : 0.0;
    out[1] = p ? particles_clamp_abs_or(p->position[1], 0.0, PARTICLES3D_WORLD_ABS_MAX) : 0.0;
    out[2] = p ? particles_clamp_abs_or(p->position[2], 0.0, PARTICLES3D_WORLD_ABS_MAX) : 0.0;
}

/// @brief Returns 1 if continuous emission is enabled (`_start` called, no subsequent `_stop`).
/// @param o Particles3D handle.
/// @return 1 when continuous emission is enabled, otherwise 0.
int8_t rt_particles3d_get_emitting(void *o) {
    rt_particles3d *p = particles3d_checked(o);
    return p && p->emitting ? 1 : 0;
}

/// @brief Enable or disable the one-update terminal-frame snapshot for expired particles.
/// @details Enabled by default for visual compatibility. Expired particles are always removed from
///   the live count in the update where their lifetime reaches zero. When enabled, their exact
///   endpoint state is retained in non-live pool slots until the next valid update so Draw can show
///   it once. Disabling the option immediately discards any pending terminal snapshots.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param enabled Nonzero to retain endpoint snapshots, zero to expire without a final draw.
void rt_particles3d_set_render_final_frame(void *o, int8_t enabled) {
    rt_particles3d *ps = particles3d_checked(o);
    if (!ps)
        return;
    ps->render_final_frame = enabled ? 1 : 0;
    if (!ps->render_final_frame)
        ps->terminal_count = 0;
}

/// @brief Return whether endpoint snapshots are retained for one terminal frame.
/// @param o Particles3D handle.
/// @return 1 when terminal-frame rendering is enabled, otherwise 0 (including invalid handles).
int8_t rt_particles3d_get_render_final_frame(void *o) {
    rt_particles3d *ps = particles3d_checked(o);
    return ps && ps->render_final_frame ? 1 : 0;
}

/// @brief Return cumulative simulation time explicitly dropped by the catch-up safety budget.
/// @details The value grows only when an Update contains more complete fixed steps than the bounded
///   call can execute. Fractional residual time is carried and is not counted as dropped.
/// @param o Particles3D handle.
/// @return Finite nonnegative seconds dropped since construction or ResetDroppedTime; zero for an
///   invalid handle.
double rt_particles3d_get_dropped_time(void *o) {
    rt_particles3d *ps = particles3d_checked(o);
    return ps && isfinite(ps->dropped_time_total) && ps->dropped_time_total > 0.0
               ? ps->dropped_time_total
               : 0.0;
}

/// @brief Return simulation time dropped by the most recent Update call.
/// @param o Particles3D handle.
/// @return Finite nonnegative dropped seconds, or zero for no drop/invalid handle.
double rt_particles3d_get_last_dropped_time(void *o) {
    rt_particles3d *ps = particles3d_checked(o);
    return ps && isfinite(ps->dropped_time_last_update) && ps->dropped_time_last_update > 0.0
               ? ps->dropped_time_last_update
               : 0.0;
}

/// @brief Return the normal fixed-step remainder carried into the next Update call.
/// @param o Particles3D handle.
/// @return Residual seconds in `[0, 1/60)`, or zero for an invalid/corrupt handle.
double rt_particles3d_get_residual_time(void *o) {
    rt_particles3d *ps = particles3d_checked(o);
    return ps && isfinite(ps->simulation_residual) && ps->simulation_residual > 0.0 &&
                   ps->simulation_residual < PARTICLES3D_SIMULATION_STEP
               ? ps->simulation_residual
               : 0.0;
}

/// @brief Reset cumulative and last-update dropped-time telemetry without changing simulation.
/// @param o Particles3D handle; invalid handles are ignored.
void rt_particles3d_reset_dropped_time(void *o) {
    rt_particles3d *ps = particles3d_checked(o);
    if (!ps)
        return;
    ps->dropped_time_total = 0.0;
    ps->dropped_time_last_update = 0.0;
}

/*==========================================================================
 * Spawning
 *=========================================================================*/

/// @brief Initialize and append one new particle to the active pool. Silently no-ops
/// when the pool is full — caller (`update`) decides emission rate, this only handles
/// the per-particle slot. Reads emitter shape (point / sphere / box), velocity
/// distribution, lifetime range, and start-colour to populate the new entry. Position
/// is emitter-origin plus a per-shape offset. Velocity is randomised within the
/// configured spread, then scaled to the speed range; lifetime gets a random value in
/// the configured min..max window so the pool desynchronises naturally.
/// @param ps Particle system receiving the new live particle.
static void spawn_particle(rt_particles3d *ps) {
    if (!ps || !ps->particles || ps->max_particles <= 0)
        return;
    if (ps->count < 0)
        ps->count = 0;
    if (ps->terminal_count < 0 || ps->terminal_count > ps->max_particles)
        ps->terminal_count = 0;
    if (ps->count >= ps->max_particles - ps->terminal_count)
        return;
    vgfx3d_particle_t *p = &ps->particles[ps->count++];

    /* Position: emitter origin + shape offset */
    p->pos[0] = particles_clamp_abs_or(ps->position[0], 0.0, PARTICLES3D_WORLD_ABS_MAX);
    p->pos[1] = particles_clamp_abs_or(ps->position[1], 0.0, PARTICLES3D_WORLD_ABS_MAX);
    p->pos[2] = particles_clamp_abs_or(ps->position[2], 0.0, PARTICLES3D_WORLD_ABS_MAX);
    double emitter_size[3] = {
        fabs(particles_clamp_abs_or(ps->emitter_size[0], 0.0, PARTICLES3D_PARAM_MAX)),
        fabs(particles_clamp_abs_or(ps->emitter_size[1], 0.0, PARTICLES3D_PARAM_MAX)),
        fabs(particles_clamp_abs_or(ps->emitter_size[2], 0.0, PARTICLES3D_PARAM_MAX)),
    };

    if (ps->emitter_shape == 1) /* sphere */
    {
        double r = cbrt((double)randf(ps)) * emitter_size[0];
        double theta = (double)randf(ps) * (2.0 * M_PI);
        double phi = acos(particles_clamp(1.0 - 2.0 * (double)randf(ps), -1.0, 1.0));
        p->pos[0] += r * sin(phi) * cos(theta);
        p->pos[1] += r * cos(phi);
        p->pos[2] += r * sin(phi) * sin(theta);
    } else if (ps->emitter_shape == 2) /* box */
    {
        p->pos[0] += ((double)randf(ps) - 0.5) * 2.0 * emitter_size[0];
        p->pos[1] += ((double)randf(ps) - 0.5) * 2.0 * emitter_size[1];
        p->pos[2] += ((double)randf(ps) - 0.5) * 2.0 * emitter_size[2];
    }
    for (int i = 0; i < 3; i++)
        p->pos[i] = particles_clamp_abs_or(p->pos[i], 0.0, PARTICLES3D_WORLD_ABS_MAX);

    /* Velocity */
    float dir[3];
    random_cone_dir(ps, ps->emit_dir, ps->emit_spread, dir);
    double speed = (double)rand_range(ps, ps->speed_min, ps->speed_max);
    p->vel[0] = (double)dir[0] * speed;
    p->vel[1] = (double)dir[1] * speed;
    p->vel[2] = (double)dir[2] * speed;

    /* Life */
    p->max_life = rand_range(ps, ps->life_min, ps->life_max);
    if (p->max_life < 0.01f)
        p->max_life = 0.01f;
    p->life = p->max_life;
    p->remaining_life = (double)p->max_life;

    /* Initial visuals */
    p->size = (float)particles_nonnegative_or_zero(ps->size_start);
    p->color[0] = ps->color_start[0];
    p->color[1] = ps->color_start[1];
    p->color[2] = ps->color_start[2];
    p->color[3] = (float)ps->alpha_start;
    if (ps->trail_pos && ps->trail_segments > 0) {
        int32_t slot = (int32_t)(p - ps->particles);
        ps->trail_age[slot] = 0.0f;
        ps->trail_len[slot] = 0;
        ps->trail_head[slot] = 0;
    }
    if (!particle_state_is_finite(p))
        ps->count--;
}

/// @brief Spawn `count` particles immediately (in addition to any continuous emission). Useful
/// for explosions, sparks, one-shot effects.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param count Requested number of particles; spawning stops when the fixed pool is full.
void rt_particles3d_burst(void *o, int64_t count) {
    rt_particles3d *ps = particles3d_checked(o);
    if (!ps || count <= 0)
        return;
    if (ps->count < 0)
        ps->count = 0;
    if (ps->count > ps->max_particles)
        ps->count = ps->max_particles;
    if (ps->terminal_count < 0 || ps->terminal_count > ps->max_particles - ps->count)
        ps->terminal_count = 0;
    int64_t available =
        (int64_t)ps->max_particles - (int64_t)ps->count - (int64_t)ps->terminal_count;
    if (available <= 0)
        return;
    if (count > available)
        count = available;
    for (int64_t i = 0; i < count; i++)
        spawn_particle(ps);
}

/// @brief Internal floating-origin hook: subtract the world rebase delta from
///   the emitter origin and every live particle, preserving velocities/lifetimes.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param dx World-origin x displacement to subtract.
/// @param dy World-origin y displacement to subtract.
/// @param dz World-origin z displacement to subtract.
void rt_particles3d_rebase_origin(void *o, double dx, double dy, double dz) {
    rt_particles3d *ps = particles3d_checked(o);
    if (!ps)
        return;
    double delta[3] = {particles_clamp_abs_or(dx, 0.0, PARTICLES3D_WORLD_ABS_MAX),
                       particles_clamp_abs_or(dy, 0.0, PARTICLES3D_WORLD_ABS_MAX),
                       particles_clamp_abs_or(dz, 0.0, PARTICLES3D_WORLD_ABS_MAX)};
    if (delta[0] == 0.0 && delta[1] == 0.0 && delta[2] == 0.0)
        return;

    ps->position[0] =
        particles_clamp_abs_or(ps->position[0] - delta[0], 0.0, PARTICLES3D_WORLD_ABS_MAX);
    ps->position[1] =
        particles_clamp_abs_or(ps->position[1] - delta[1], 0.0, PARTICLES3D_WORLD_ABS_MAX);
    ps->position[2] =
        particles_clamp_abs_or(ps->position[2] - delta[2], 0.0, PARTICLES3D_WORLD_ABS_MAX);

    for (int32_t i = 0; i < ps->count;) {
        double x = ps->particles[i].pos[0] - delta[0];
        double y = ps->particles[i].pos[1] - delta[1];
        double z = ps->particles[i].pos[2] - delta[2];
        if (!isfinite(x) || !isfinite(y) || !isfinite(z) || fabs(x) > PARTICLES3D_WORLD_ABS_MAX ||
            fabs(y) > PARTICLES3D_WORLD_ABS_MAX || fabs(z) > PARTICLES3D_WORLD_ABS_MAX) {
            particles3d_swap_kill(ps, i);
            continue;
        }
        ps->particles[i].pos[0] = x;
        ps->particles[i].pos[1] = y;
        ps->particles[i].pos[2] = z;
        i++;
    }
    for (int32_t i = 0; i < ps->terminal_count; i++) {
        int32_t slot = particles3d_terminal_slot(ps, i);
        if (slot < 0) {
            ps->terminal_count = 0;
            break;
        }
        vgfx3d_particle_t *terminal = &ps->particles[slot];
        terminal->pos[0] =
            particles_clamp_abs_or(terminal->pos[0] - delta[0], 0.0, PARTICLES3D_WORLD_ABS_MAX);
        terminal->pos[1] =
            particles_clamp_abs_or(terminal->pos[1] - delta[1], 0.0, PARTICLES3D_WORLD_ABS_MAX);
        terminal->pos[2] =
            particles_clamp_abs_or(terminal->pos[2] - delta[2], 0.0, PARTICLES3D_WORLD_ABS_MAX);
    }
}

/*==========================================================================
 * Update (Euler integration + lifetime + interpolation)
 *=========================================================================*/

/// @brief Sanitized visual and acceleration inputs shared by every substep in one public update.
typedef struct {
    double gravity[3];
    double size_start;
    double size_end;
    float color_start[3];
    float color_end[3];
    double alpha_start;
    double alpha_end;
} particles3d_step_config;

/// @brief Snapshot and sanitize emitter parameters once for a bounded substep batch.
/// @param ps Particle system whose authored parameters are read.
/// @param out_config Caller-owned output written with finite bounded values.
static void particles3d_prepare_step_config(const rt_particles3d *ps,
                                            particles3d_step_config *out_config) {
    if (!ps || !out_config)
        return;
    for (int c = 0; c < 3; c++) {
        out_config->gravity[c] = particles_clamp_abs_or(ps->gravity[c], 0.0, PARTICLES3D_PARAM_MAX);
        out_config->color_start[c] = (float)particles_clamp((double)ps->color_start[c], 0.0, 1.0);
        out_config->color_end[c] = (float)particles_clamp((double)ps->color_end[c], 0.0, 1.0);
    }
    out_config->size_start = particles_nonnegative_or_zero(ps->size_start);
    out_config->size_end = particles_nonnegative_or_zero(ps->size_end);
    out_config->alpha_start = particles_clamp(ps->alpha_start, 0.0, 1.0);
    out_config->alpha_end = particles_clamp(ps->alpha_end, 0.0, 1.0);
}

/// @brief Apply the size/color/alpha envelope for one normalized lifetime age.
/// @param p Particle receiving the visual state.
/// @param config Sanitized update-batch configuration.
/// @param age Normalized age, clamped internally to `[0, 1]`.
static void particles3d_apply_visual_envelope(vgfx3d_particle_t *p,
                                              const particles3d_step_config *config,
                                              double age) {
    if (!p || !config)
        return;
    age = particles_clamp(age, 0.0, 1.0);
    p->size = (float)(config->size_start + age * (config->size_end - config->size_start));
    for (int c = 0; c < 3; c++)
        p->color[c] =
            (float)((double)config->color_start[c] +
                    age * ((double)config->color_end[c] - (double)config->color_start[c]));
    p->color[3] = (float)(config->alpha_start + age * (config->alpha_end - config->alpha_start));
}

/// @brief Advance one live particle's ribbon history across an exact motion segment.
/// @details Control points are interpolated between caller-provided pre/post positions, so an
///   expiring particle never extrapolates beyond its remaining lifetime and fixed-step partitioning
///   produces stable sampling cadence.
/// @param ps Particle system owning parallel trail arrays.
/// @param slot Live particle/trail slot index.
/// @param previous Position before the motion segment.
/// @param current Position after the motion segment.
/// @param elapsed Exact simulated seconds in the motion segment.
static void particles3d_advance_trail(rt_particles3d *ps,
                                      int32_t slot,
                                      const double previous[3],
                                      const double current[3],
                                      double elapsed) {
    if (!ps || !previous || !current || slot < 0 || slot >= ps->count || !ps->trail_pos ||
        ps->trail_segments <= 0 || ps->trail_lifetime <= 0.0f || !isfinite(elapsed) ||
        elapsed <= 0.0)
        return;
    float spacing = ps->trail_lifetime / (float)ps->trail_segments;
    ps->trail_age[slot] += (float)elapsed;
    if (ps->trail_age[slot] < spacing && ps->trail_len[slot] != 0)
        return;
    size_t stride = (size_t)ps->trail_segments * 3u;
    int32_t emit = 1;
    if (ps->trail_len[slot] != 0 && spacing > 0.0f) {
        float intervals = ps->trail_age[slot] / spacing;
        if (isfinite(intervals) && intervals > 1.0f)
            emit = intervals >= (float)ps->trail_segments ? ps->trail_segments : (int32_t)intervals;
    }
    for (int32_t e = 1; e <= emit; e++) {
        double t = (double)e / (double)emit;
        int16_t head = ps->trail_head[slot];
        float *dst = &ps->trail_pos[(size_t)slot * stride + (size_t)head * 3u];
        for (int c = 0; c < 3; c++)
            dst[c] = (float)(previous[c] + (current[c] - previous[c]) * t);
        ps->trail_head[slot] = (int16_t)((head + 1) % ps->trail_segments);
        if (ps->trail_len[slot] < (int16_t)ps->trail_segments)
            ps->trail_len[slot]++;
    }
    if (ps->trail_len[slot] != 0 && spacing > 0.0f) {
        float remainder = ps->trail_age[slot] - (float)emit * spacing;
        ps->trail_age[slot] = (isfinite(remainder) && remainder > 0.0f) ? remainder : 0.0f;
    } else {
        ps->trail_age[slot] = 0.0f;
    }
}

/// @brief Simulate one fixed particle-system substep and emit rate-driven particles afterward.
/// @details Every live particle integrates for `min(dt, remaining_lifetime)`. An expiring particle
///   reaches its exact position/velocity/envelope endpoint, is removed from the live prefix in this
///   substep, and may be copied to the terminal snapshot. Work is O(live particles + spawned count)
///   with no allocation.
/// @param ps Particle system to mutate.
/// @param dt Positive finite fixed-step duration.
/// @param config Sanitized configuration shared by the enclosing public update.
static void particles3d_simulate_substep(rt_particles3d *ps,
                                         double dt,
                                         const particles3d_step_config *config) {
    if (!ps || !config || !isfinite(dt) || dt <= 0.0)
        return;
    for (int32_t i = 0; i < ps->count;) {
        vgfx3d_particle_t *p = &ps->particles[i];
        if (!particle_state_is_finite(p) || p->remaining_life <= 0.0) {
            particles3d_swap_kill(ps, i);
            continue;
        }
        double remaining = p->remaining_life;
        double active_dt = dt < remaining ? dt : remaining;
        int expires = dt >= remaining;
        double previous[3] = {p->pos[0], p->pos[1], p->pos[2]};
        for (int c = 0; c < 3; c++) {
            p->pos[c] = particles_clamp_abs_or(
                p->pos[c] + p->vel[c] * active_dt, 0.0, PARTICLES3D_WORLD_ABS_MAX);
            p->vel[c] = particles_clamp_abs_or(
                p->vel[c] + config->gravity[c] * active_dt, 0.0, PARTICLES3D_PARAM_MAX);
        }
        p->remaining_life = expires ? 0.0 : remaining - active_dt;
        p->life = (float)p->remaining_life;
        particles3d_advance_trail(ps, i, previous, p->pos, active_dt);
        double age = expires ? 1.0 : 1.0 - p->remaining_life / (double)p->max_life;
        particles3d_apply_visual_envelope(p, config, age);
        if (!particle_state_is_finite(p)) {
            particles3d_swap_kill(ps, i);
            continue;
        }
        if (expires) {
            vgfx3d_particle_t endpoint = *p;
            particles3d_swap_kill(ps, i);
            particles3d_retain_terminal_particle(ps, &endpoint);
            continue;
        }
        i++;
    }

    ps->rate = particles_nonnegative_or_zero(ps->rate);
    if (ps->emitting && ps->rate > 0.0) {
        int32_t available = ps->max_particles - ps->count - ps->terminal_count;
        if (available < 0)
            available = 0;
        double max_budget = (double)available + 0.999999;
        if (!isfinite(ps->accumulator) || ps->accumulator < 0.0)
            ps->accumulator = 0.0;
        ps->accumulator += ps->rate * dt;
        if (!isfinite(ps->accumulator) || ps->accumulator > max_budget)
            ps->accumulator = max_budget;
        /* Repeated binary approximations of rate / 60 can land a few ulps below an
         * integer emission boundary.  The tolerance is many orders of magnitude
         * smaller than one particle, but preserves exact authored rates such as
         * four particles over thirty fixed substeps. */
        while (ps->accumulator >= 1.0 - 1e-12 &&
               ps->count + ps->terminal_count < ps->max_particles) {
            spawn_particle(ps);
            ps->accumulator -= 1.0;
            if (ps->accumulator < 0.0 && ps->accumulator > -1e-12)
                ps->accumulator = 0.0;
        }
    }
}

/// @brief Advance the emitter through bounded 60 Hz fixed substeps.
/// @details Normal fractional time is retained in `ResidualTime`. At most 60 complete substeps are
///   executed per call. Additional complete-step time is explicitly accumulated in `DroppedTime`
///   and `LastDroppedTime`; only the fractional remainder is carried. Terminal snapshots from the
///   previous valid update are discarded before this update begins.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param delta_time Positive finite elapsed seconds. Invalid/nonpositive values perform no update.
void rt_particles3d_update(void *o, double delta_time) {
    rt_particles3d *ps = particles3d_checked(o);
    if (!ps)
        return;
    ps->dropped_time_last_update = 0.0;
    if (!isfinite(delta_time) || delta_time <= 0.0 || !ps->particles || ps->max_particles <= 0)
        return;
    if (ps->count < 0)
        ps->count = 0;
    if (ps->count > ps->max_particles)
        ps->count = ps->max_particles;
    if (ps->terminal_count < 0 || ps->terminal_count > ps->max_particles - ps->count)
        ps->terminal_count = 0;
    /* A terminal snapshot represents exactly the interval between two valid Update calls. */
    ps->terminal_count = 0;
    if (!isfinite(ps->simulation_residual) || ps->simulation_residual < 0.0 ||
        ps->simulation_residual >= PARTICLES3D_SIMULATION_STEP)
        ps->simulation_residual = 0.0;

    double total = delta_time + ps->simulation_residual;
    if (!isfinite(total) || total < delta_time)
        total = DBL_MAX;
    double ratio = total / PARTICLES3D_SIMULATION_STEP;
    int32_t substeps = 0;
    if (ratio >= (double)PARTICLES3D_MAX_SUBSTEPS) {
        substeps = PARTICLES3D_MAX_SUBSTEPS;
    } else if (ratio >= 1.0 - 1e-12) {
        substeps = (int32_t)floor(ratio + 1e-12);
    }
    if (substeps < 0)
        substeps = 0;
    if (substeps > PARTICLES3D_MAX_SUBSTEPS)
        substeps = PARTICLES3D_MAX_SUBSTEPS;

    double consumed = (double)substeps * PARTICLES3D_SIMULATION_STEP;
    double remaining = total - consumed;
    if (!isfinite(remaining) || remaining < 0.0)
        remaining = 0.0;
    double dropped = 0.0;
    if (substeps == PARTICLES3D_MAX_SUBSTEPS && remaining >= PARTICLES3D_SIMULATION_STEP) {
        double carried = fmod(remaining, PARTICLES3D_SIMULATION_STEP);
        if (!isfinite(carried) || carried < 1e-12 || PARTICLES3D_SIMULATION_STEP - carried < 1e-12)
            carried = 0.0;
        dropped = remaining - carried;
        ps->simulation_residual = carried;
    } else {
        ps->simulation_residual = remaining;
        if (ps->simulation_residual >= PARTICLES3D_SIMULATION_STEP)
            ps->simulation_residual = fmod(ps->simulation_residual, PARTICLES3D_SIMULATION_STEP);
    }
    if (dropped > 0.0 && isfinite(dropped)) {
        ps->dropped_time_last_update = dropped;
        if (!isfinite(ps->dropped_time_total) || ps->dropped_time_total < 0.0)
            ps->dropped_time_total = 0.0;
        if (dropped > DBL_MAX - ps->dropped_time_total)
            ps->dropped_time_total = DBL_MAX;
        else
            ps->dropped_time_total += dropped;
    }

    particles3d_step_config config;
    particles3d_prepare_step_config(ps, &config);
    for (int32_t step = 0; step < substeps; step++)
        particles3d_simulate_substep(ps, PARTICLES3D_SIMULATION_STEP, &config);
}

/*==========================================================================
 * Billboard rendering
 *=========================================================================*/

extern void rt_canvas3d_draw_mesh(void *obj, void *mesh, void *transform, void *material);
extern void rt_canvas3d_draw_mesh_matrix_keyed(void *obj,
                                               void *mesh_obj,
                                               const double *model_matrix,
                                               void *material_obj,
                                               const void *motion_key,
                                               const float *prev_bone_palette,
                                               const float *prev_morph_weights);
extern int rt_canvas3d_get_camera_relative_origin(void *canvas, double out_origin[3]);
extern void *rt_material3d_new(void);
extern void rt_material3d_set_color(void *m, double r, double g, double b);
extern void rt_material3d_set_unlit(void *m, int8_t u);
extern void rt_material3d_set_alpha(void *m, double a);
extern void rt_material3d_set_alpha_mode(void *m, int64_t mode);
extern void rt_material3d_set_texture(void *m, void *tex);
extern int rt_canvas3d_remove_temp_buffer(void *canvas, void *buffer);

/// @brief Sort record pairing a particle's index with its camera-space depth.
/// @details Sorting by the camera forward axis is more stable for large billboard quads than
///   Euclidean distance, which can flip ordering near screen edges.
typedef struct particle3d_sort_key {
    int32_t index;
    double view_depth;
} particle3d_sort_key;

/// @brief Compare two particle sort keys for back-to-front order.
/// @details Higher camera-space depth sorts first. Equal-depth ties use the stable particle index
///   so alpha ordering remains deterministic across platforms and frames.
/// @param ka Left sort key.
/// @param kb Right sort key.
/// @return Negative when @p ka precedes @p kb, positive when it follows, or zero when identical.
static int particle3d_sort_key_compare_desc(const particle3d_sort_key *ka,
                                            const particle3d_sort_key *kb) {
    if (!ka || !kb)
        return 0;
    if (ka->view_depth < kb->view_depth)
        return 1;
    if (ka->view_depth > kb->view_depth)
        return -1;
    if (ka->index < kb->index)
        return -1;
    if (ka->index > kb->index)
        return 1;
    return 0;
}

/// @brief Return true when sort keys are already in back-to-front draw order.
/// @details Particle systems often move coherently frame-to-frame, so the previous draw order
///          remains sorted for small camera/particle deltas. Detecting that case avoids a full
///          merge pass and keeps deterministic tie ordering by comparing index values too.
/// @param keys Sort-key array to inspect.
/// @param count Number of keys in @p keys.
/// @return 1 when the array is already descending by depth with ascending index ties.
static int particles3d_sort_keys_already_descending(const particle3d_sort_key *keys,
                                                    int32_t count) {
    if (!keys || count <= 1)
        return 1;
    for (int32_t i = 1; i < count; i++) {
        const particle3d_sort_key *prev = &keys[i - 1];
        const particle3d_sort_key *cur = &keys[i];
        if (prev->view_depth < cur->view_depth)
            return 0;
        if (prev->view_depth == cur->view_depth && prev->index > cur->index)
            return 0;
    }
    return 1;
}

/// @brief Sort a small particle key array with insertion sort.
/// @details For the common case of a few dozen transparent particles, insertion sort avoids the
///          merge pass overhead and performs well on nearly sorted input.
/// @param keys Sort-key array to reorder in place.
/// @param count Number of keys in @p keys.
static void particles3d_insertion_sort_keys_desc(particle3d_sort_key *keys, int32_t count) {
    if (!keys || count <= 1)
        return;
    for (int32_t i = 1; i < count; i++) {
        particle3d_sort_key key = keys[i];
        int32_t j = i - 1;
        while (j >= 0) {
            if (particle3d_sort_key_compare_desc(&keys[j], &key) <= 0)
                break;
            keys[j + 1] = keys[j];
            j--;
        }
        keys[j + 1] = key;
    }
}

/// @brief Merge two sorted particle-key runs from @p src into @p dst.
/// @details Stable merge preserves index tie ordering and avoids libc qsort's indirect comparator
///   overhead for large alpha-blended emitters.
/// @param src Source array containing both ordered runs.
/// @param dst Destination array receiving the merged run.
/// @param left Inclusive start index of the first run.
/// @param mid Exclusive end of the first run and inclusive start of the second.
/// @param right Exclusive end index of the second run.
static void particles3d_merge_sort_run(const particle3d_sort_key *src,
                                       particle3d_sort_key *dst,
                                       int32_t left,
                                       int32_t mid,
                                       int32_t right) {
    int32_t i = left;
    int32_t j = mid;
    int32_t out = left;
    while (i < mid && j < right) {
        if (particle3d_sort_key_compare_desc(&src[i], &src[j]) <= 0)
            dst[out++] = src[i++];
        else
            dst[out++] = src[j++];
    }
    while (i < mid)
        dst[out++] = src[i++];
    while (j < right)
        dst[out++] = src[j++];
}

/// @brief Sort particle keys back-to-front using persistent scratch storage.
/// @details Skips work when keys are already ordered, uses insertion sort for small batches, and
///          uses a stable bottom-up merge sort for larger unordered alpha-blended emitters.
/// @param keys Sort-key array to reorder in place.
/// @param scratch Temporary array with at least @p count keys for the merge path.
/// @param count Number of keys in @p keys.
static void particles3d_sort_keys_back_to_front(particle3d_sort_key *keys,
                                                particle3d_sort_key *scratch,
                                                int32_t count) {
    if (!keys || count <= 1 || particles3d_sort_keys_already_descending(keys, count))
        return;
    if (count <= 32 || !scratch) {
        particles3d_insertion_sort_keys_desc(keys, count);
        return;
    }
    particle3d_sort_key *src = keys;
    particle3d_sort_key *dst = scratch;
    int32_t width = 1;
    while (width < count) {
        for (int32_t left = 0; left < count; left += width * 2) {
            int32_t mid = left + width;
            int32_t right = left + width * 2;
            if (mid > count)
                mid = count;
            if (right > count)
                right = count;
            particles3d_merge_sort_run(src, dst, left, mid, right);
        }
        particle3d_sort_key *tmp = src;
        src = dst;
        dst = tmp;
        if (width > count / 2)
            break;
        width *= 2;
    }
    if (src != keys)
        memcpy(keys, src, (size_t)count * sizeof(*keys));
}

/// @brief Ensure the persistent particle-sort scratch buffer holds @p count keys.
/// @details Returning failure aborts alpha particle rendering for the frame instead of silently
///   falling back to unsorted transparent quads, because that fallback causes obvious flicker.
/// @param ps Particle system whose primary and scratch sort arrays may be grown.
/// @param count Number of sort keys required by the pending draw.
/// @return Nonzero when both arrays can hold @p count keys, otherwise zero after trapping on
/// overflow or allocation failure.
static int particles3d_ensure_sort_keys(rt_particles3d *ps, int32_t count) {
    void *grown;
    void *scratch_grown;
    int32_t target_capacity;
    if (!ps || count <= 0)
        return 0;
    target_capacity = ps->max_particles > count ? ps->max_particles : count;
    if (target_capacity <= 0)
        return 0;
    if (ps->sort_key_capacity >= target_capacity && ps->sort_keys && ps->sort_scratch)
        return 1;
    if ((size_t)target_capacity > SIZE_MAX / sizeof(particle3d_sort_key)) {
        rt_trap("Particles3D.Draw: sort key allocation overflow");
        return 0;
    }
    grown = realloc(ps->sort_keys, (size_t)target_capacity * sizeof(particle3d_sort_key));
    scratch_grown =
        realloc(ps->sort_scratch, (size_t)target_capacity * sizeof(particle3d_sort_key));
    if (!grown || !scratch_grown) {
        if (grown)
            ps->sort_keys = grown;
        if (scratch_grown)
            ps->sort_scratch = scratch_grown;
        rt_trap("Particles3D.Draw: sort key allocation failed");
        return 0;
    }
    ps->sort_keys = grown;
    ps->sort_scratch = scratch_grown;
    ps->sort_key_capacity = target_capacity;
    ps->sort_key_grow_count++;
    return 1;
}

/// @brief Test hook: current persistent alpha-sort key capacity.
/// @param o Particles3D handle.
/// @return Current retained sort-key capacity, or zero for an invalid handle.
int64_t rt_particles3d_test_sort_key_capacity(void *o) {
    rt_particles3d *ps = particles3d_checked(o);
    return ps ? ps->sort_key_capacity : 0;
}

/// @brief Test hook: number of persistent alpha-sort key buffer growth operations.
/// @param o Particles3D handle.
/// @return Number of successful persistent sort-buffer growth operations, or zero if invalid.
uint64_t rt_particles3d_test_sort_key_grow_count(void *o) {
    rt_particles3d *ps = particles3d_checked(o);
    return ps ? ps->sort_key_grow_count : 0;
}

/// @brief Ensure the emitter-owned compact particle instance scratch can hold @p count records.
/// @details Capacity grows geometrically from a small initial block and never exceeds the emitter
///   pool limit. The allocation is reused across cameras and frames; Canvas3D copies the active
///   prefix when a draw is queued, so later fills cannot mutate an earlier deferred command.
/// @param ps Particle emitter whose private scratch allocation is grown.
/// @param count Number of compact instance records required by the pending draw.
/// @return Non-zero when at least @p count records are writable, otherwise zero after trapping on
///   overflow or allocation failure.
static int particles3d_ensure_instance_scratch(rt_particles3d *ps, int32_t count) {
    int32_t target_capacity;
    vgfx3d_particle_instance_t *grown;
    if (!ps || count <= 0 || count > ps->max_particles)
        return 0;
    if (ps->instance_scratch && ps->instance_scratch_capacity >= count)
        return 1;

    target_capacity = ps->instance_scratch_capacity > 0 ? ps->instance_scratch_capacity : 64;
    if (target_capacity > ps->max_particles)
        target_capacity = ps->max_particles;
    while (target_capacity < count) {
        if (target_capacity > ps->max_particles / 2) {
            target_capacity = ps->max_particles;
            break;
        }
        target_capacity *= 2;
    }
    if (target_capacity < count ||
        (size_t)target_capacity > SIZE_MAX / sizeof(*ps->instance_scratch)) {
        rt_trap("Particles3D.Draw: compact instance allocation overflow");
        return 0;
    }
    grown = (vgfx3d_particle_instance_t *)realloc(ps->instance_scratch,
                                                  (size_t)target_capacity * sizeof(*grown));
    if (!grown) {
        rt_trap("Particles3D.Draw: compact instance allocation failed");
        return 0;
    }
    ps->instance_scratch = grown;
    ps->instance_scratch_capacity = target_capacity;
    ps->instance_scratch_grow_count++;
    return 1;
}

/// @brief Test hook returning the retained compact-instance scratch capacity.
/// @details This is intentionally absent from the public runtime registry; contract tests use it
///   to prove that repeated hardware draws do not allocate or rebuild CPU billboard geometry.
/// @param o Particles3D handle.
/// @return Current compact-instance capacity, or zero for an invalid handle.
int64_t rt_particles3d_test_instance_scratch_capacity(void *o) {
    rt_particles3d *ps = particles3d_checked(o);
    return ps ? ps->instance_scratch_capacity : 0;
}

/// @brief Test hook returning the number of compact-instance scratch growth operations.
/// @details The counter changes only after a successful realloc and therefore distinguishes
///   retained-buffer reuse from per-frame scratch reconstruction.
/// @param o Particles3D handle.
/// @return Number of successful compact-instance scratch growth operations, or zero if invalid.
uint64_t rt_particles3d_test_instance_scratch_grow_count(void *o) {
    rt_particles3d *ps = particles3d_checked(o);
    return ps ? ps->instance_scratch_grow_count : 0;
}

/// @brief Lazily create the system's shared unlit white particle material in @p *slot.
/// @param slot Address of the retained material slot to validate or initialize.
/// @return 1 if the slot holds a material (existing or newly made), 0 on allocation failure.
static int particles3d_ensure_material(void **slot) {
    if (!slot)
        return 0;
    if (*slot && !rt_g3d_has_class(*slot, RT_G3D_MATERIAL3D_CLASS_ID))
        particles3d_release_material_slot(slot);
    if (!*slot) {
        *slot = rt_material3d_new();
        if (!*slot)
            return 0;
        rt_material3d_set_color(*slot, 1.0, 1.0, 1.0);
        rt_material3d_set_unlit(*slot, 1);
    }
    return 1;
}

/// @brief Ensure the growable overflow draw-slot arrays can address @p needed slots.
/// @details Fixed draw slots cover the common one/few camera draws per frame. When a caller draws
///   the same emitter more often in a frame, overflow slots grow once and are reused on later
///   frames instead of allocating canvas-owned transient buffers every draw.
/// @param ps Particle system owning the overflow slot arrays.
/// @param needed Number of overflow slots required for the current frame.
/// @return 1 when the overflow slot table is large enough, 0 on overflow/allocation failure.
static int particles3d_ensure_overflow_draw_slots(rt_particles3d *ps, int32_t needed) {
    int32_t old_capacity;
    int32_t new_capacity;
    if (!ps || needed < 0 || ps->overflow_draw_slot_capacity < 0)
        return 0;
    if (needed <= ps->overflow_draw_slot_capacity)
        return 1;
    old_capacity = ps->overflow_draw_slot_capacity;
    new_capacity = old_capacity > 0 ? old_capacity : 4;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(*ps->overflow_draw_vertices) ||
        (size_t)new_capacity > SIZE_MAX / sizeof(*ps->overflow_draw_indices) ||
        (size_t)new_capacity > SIZE_MAX / sizeof(*ps->overflow_draw_vertex_capacity) ||
        (size_t)new_capacity > SIZE_MAX / sizeof(*ps->overflow_draw_index_capacity) ||
        (size_t)new_capacity > SIZE_MAX / sizeof(*ps->overflow_draw_materials))
        return 0;

    vgfx3d_vertex_t **vertices = (vgfx3d_vertex_t **)realloc(
        ps->overflow_draw_vertices, (size_t)new_capacity * sizeof(*vertices));
    if (!vertices)
        return 0;
    ps->overflow_draw_vertices = vertices;
    uint32_t **indices =
        (uint32_t **)realloc(ps->overflow_draw_indices, (size_t)new_capacity * sizeof(*indices));
    if (!indices)
        return 0;
    ps->overflow_draw_indices = indices;
    uint32_t *vertex_caps = (uint32_t *)realloc(ps->overflow_draw_vertex_capacity,
                                                (size_t)new_capacity * sizeof(*vertex_caps));
    if (!vertex_caps)
        return 0;
    ps->overflow_draw_vertex_capacity = vertex_caps;
    uint32_t *index_caps = (uint32_t *)realloc(ps->overflow_draw_index_capacity,
                                               (size_t)new_capacity * sizeof(*index_caps));
    if (!index_caps)
        return 0;
    ps->overflow_draw_index_capacity = index_caps;
    void **materials =
        (void **)realloc(ps->overflow_draw_materials, (size_t)new_capacity * sizeof(*materials));
    if (!materials)
        return 0;
    ps->overflow_draw_materials = materials;

    if (new_capacity > old_capacity) {
        size_t added = (size_t)(new_capacity - old_capacity);
        memset(ps->overflow_draw_vertices + old_capacity, 0, added * sizeof(*vertices));
        memset(ps->overflow_draw_indices + old_capacity, 0, added * sizeof(*indices));
        memset(ps->overflow_draw_vertex_capacity + old_capacity, 0, added * sizeof(*vertex_caps));
        memset(ps->overflow_draw_index_capacity + old_capacity, 0, added * sizeof(*index_caps));
        memset(ps->overflow_draw_materials + old_capacity, 0, added * sizeof(*materials));
    }
    ps->overflow_draw_slot_capacity = new_capacity;
    return 1;
}

/// @brief Prepare one reusable draw slot's vertex/index buffers and material.
/// @details Used by both fixed and overflow slot arrays. Vertex payloads are fully initialized by
///   the billboard/trail emitters, so this avoids a full-buffer zero-fill on every draw.
/// @param draw_vertices Array of retained vertex-buffer pointers.
/// @param draw_indices Array of retained index-buffer pointers.
/// @param vertex_capacity Per-slot vertex capacities corresponding to @p draw_vertices.
/// @param index_capacity Per-slot index capacities corresponding to @p draw_indices.
/// @param draw_materials Array of retained material slots.
/// @param slot Slot index to prepare.
/// @param vert_count Number of vertices required.
/// @param idx_count Number of indices required.
/// @param out_vertices Receives the writable vertex buffer.
/// @param out_indices Receives the writable index buffer.
/// @param out_material Receives the retained material handle.
/// @return 1 with output pointers assigned, 0 on allocation/material failure.
static int particles3d_prepare_draw_slot(vgfx3d_vertex_t **draw_vertices,
                                         uint32_t **draw_indices,
                                         uint32_t *vertex_capacity,
                                         uint32_t *index_capacity,
                                         void **draw_materials,
                                         int32_t slot,
                                         uint32_t vert_count,
                                         uint32_t idx_count,
                                         vgfx3d_vertex_t **out_vertices,
                                         uint32_t **out_indices,
                                         void **out_material) {
    if (!draw_vertices || !draw_indices || !vertex_capacity || !index_capacity || !draw_materials ||
        slot < 0 || !out_vertices || !out_indices || !out_material)
        return 0;
    if (vert_count > vertex_capacity[slot]) {
        vgfx3d_vertex_t *grown;
        if ((size_t)vert_count > SIZE_MAX / sizeof(*grown))
            return 0;
        grown =
            (vgfx3d_vertex_t *)realloc(draw_vertices[slot], (size_t)vert_count * sizeof(*grown));
        if (!grown)
            return 0;
        draw_vertices[slot] = grown;
        vertex_capacity[slot] = vert_count;
    }
    if (idx_count > index_capacity[slot]) {
        uint32_t *grown;
        if ((size_t)idx_count > SIZE_MAX / sizeof(*grown))
            return 0;
        grown = (uint32_t *)realloc(draw_indices[slot], (size_t)idx_count * sizeof(*grown));
        if (!grown)
            return 0;
        draw_indices[slot] = grown;
        index_capacity[slot] = idx_count;
    }
    if (!particles3d_ensure_material(&draw_materials[slot]))
        return 0;
    *out_vertices = draw_vertices[slot];
    *out_indices = draw_indices[slot];
    *out_material = draw_materials[slot];
    return 1;
}

/// @brief Fill vertex attributes that particle billboard/trail emission does not vary.
/// @details Particle quads do not use secondary UVs, tangents, or skinning, but the draw path may
///   still hash/copy the whole `vgfx3d_vertex_t`. Writing these fields explicitly keeps the
///   reusable draw buffer deterministic without a per-frame memset.
/// @param v Vertex whose secondary attributes are initialized.
static void particles3d_finalize_draw_vertex(vgfx3d_vertex_t *v) {
    if (!v)
        return;
    v->uv1[0] = v->uv[0];
    v->uv1[1] = v->uv[1];
    v->tangent[0] = 1.0f;
    v->tangent[1] = 0.0f;
    v->tangent[2] = 0.0f;
    v->tangent[3] = 1.0f;
    for (int i = 0; i < 4; i++) {
        v->bone_indices[i] = 0;
        v->bone_weights[i] = 0.0f;
    }
}

/// @brief Write one fully-initialized transparent degenerate vertex for unused trail capacity.
/// @param v Destination vertex.
/// @param forward Optional surface normal; the positive z axis is used when NULL.
static void particles3d_write_degenerate_vertex(vgfx3d_vertex_t *v, const float forward[3]) {
    if (!v)
        return;
    memset(v->pos, 0, sizeof(v->pos));
    v->normal[0] = forward ? forward[0] : 0.0f;
    v->normal[1] = forward ? forward[1] : 0.0f;
    v->normal[2] = forward ? forward[2] : 1.0f;
    v->uv[0] = 0.0f;
    v->uv[1] = 0.0f;
    v->color[0] = 0.0f;
    v->color[1] = 0.0f;
    v->color[2] = 0.0f;
    v->color[3] = 0.0f;
    particles3d_finalize_draw_vertex(v);
}

/// @brief Acquire transient vertex/index/material storage for this frame's particle batch.
/// @details Uses fixed reusable slots first, then grows reusable overflow slots for additional
///          same-frame draws. @p out_canvas_owned is retained for the old cleanup contract and is
///          always false on success.
/// @param ps Particle system owning reusable draw slots.
/// @param canvas Active Canvas3D state whose frame serial scopes slot reuse.
/// @param vert_count Number of CPU vertices required.
/// @param idx_count Number of CPU indices required.
/// @param out_vertices Receives writable vertex storage.
/// @param out_indices Receives writable index storage.
/// @param out_material Receives the material dedicated to this draw slot.
/// @param out_canvas_owned Receives zero because successful storage is emitter-owned.
/// @return 1 with the out-params set, 0 on invalid args or allocation failure.
static int particles3d_acquire_draw_storage(rt_particles3d *ps,
                                            rt_canvas3d *canvas,
                                            uint32_t vert_count,
                                            uint32_t idx_count,
                                            vgfx3d_vertex_t **out_vertices,
                                            uint32_t **out_indices,
                                            void **out_material,
                                            int *out_canvas_owned) {
    int32_t slot;
    int64_t frame_serial;
    if (!ps || !canvas || !out_vertices || !out_indices || !out_material || !out_canvas_owned)
        return 0;
    *out_vertices = NULL;
    *out_indices = NULL;
    *out_material = NULL;
    *out_canvas_owned = 0;
    frame_serial = canvas->frame_serial;
    if (ps->draw_frame_serial != frame_serial) {
        ps->draw_frame_serial = frame_serial;
        ps->draw_slots_used = 0;
    }
    if (ps->draw_slots_used < 0 || ps->draw_slots_used == INT32_MAX)
        return 0;
    slot = ps->draw_slots_used++;
    if (slot < PARTICLES3D_DRAW_SLOT_COUNT)
        return particles3d_prepare_draw_slot(ps->draw_vertices,
                                             ps->draw_indices,
                                             ps->draw_vertex_capacity,
                                             ps->draw_index_capacity,
                                             ps->draw_materials,
                                             slot,
                                             vert_count,
                                             idx_count,
                                             out_vertices,
                                             out_indices,
                                             out_material);

    slot -= PARTICLES3D_DRAW_SLOT_COUNT;
    if (!particles3d_ensure_overflow_draw_slots(ps, slot + 1))
        return 0;
    return particles3d_prepare_draw_slot(ps->overflow_draw_vertices,
                                         ps->overflow_draw_indices,
                                         ps->overflow_draw_vertex_capacity,
                                         ps->overflow_draw_index_capacity,
                                         ps->overflow_draw_materials,
                                         slot,
                                         vert_count,
                                         idx_count,
                                         out_vertices,
                                         out_indices,
                                         out_material);
}

/// @brief Build a row-major model matrix that translates by @p origin (identity rotation/scale).
/// @param origin Three-component translation, or NULL for the identity matrix.
/// @param out Sixteen-element output array receiving the row-major matrix.
static void particles3d_origin_model_matrix(const double origin[3], double out[16]) {
    static const double identity[16] = {
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };
    memcpy(out, identity, sizeof(identity));
    if (origin) {
        out[3] = particles_clamp_abs_or(origin[0], 0.0, PARTICLES3D_WORLD_ABS_MAX);
        out[7] = particles_clamp_abs_or(origin[1], 0.0, PARTICLES3D_WORLD_ABS_MAX);
        out[11] = particles_clamp_abs_or(origin[2], 0.0, PARTICLES3D_WORLD_ABS_MAX);
    }
}

/// @brief Normalize a 3-vector or replace it with a caller-provided fallback.
/// @param v Three-component vector to sanitize and normalize.
/// @param fx Fallback x component.
/// @param fy Fallback y component.
/// @param fz Fallback z component.
static void particles3d_normalize3_or(float v[3], float fx, float fy, float fz) {
    if (!v) {
        return;
    }
    v[0] = (float)particles_clamp_abs_or((double)v[0], (double)fx, PARTICLES3D_PARAM_MAX);
    v[1] = (float)particles_clamp_abs_or((double)v[1], (double)fy, PARTICLES3D_PARAM_MAX);
    v[2] = (float)particles_clamp_abs_or((double)v[2], (double)fz, PARTICLES3D_PARAM_MAX);
    float max_component = fmaxf(fabsf(v[0]), fmaxf(fabsf(v[1]), fabsf(v[2])));
    if (!isfinite(max_component) || max_component <= 1e-8f) {
        v[0] = fx;
        v[1] = fy;
        v[2] = fz;
        return;
    }
    float sx = v[0] / max_component;
    float sy = v[1] / max_component;
    float sz = v[2] / max_component;
    float len = sqrtf(sx * sx + sy * sy + sz * sz);
    if (!isfinite(len) || len <= 1e-8f) {
        v[0] = fx;
        v[1] = fy;
        v[2] = fz;
        return;
    }
    v[0] = sx / len;
    v[1] = sy / len;
    v[2] = sz / len;
}

/// @brief Compute the camera-plane half axes for one particle billboard.
/// @details The default axes are camera right/up scaled by half the particle size. When velocity
///   stretch is enabled, velocity is projected into the camera plane, normalized as the long axis,
///   and capped at 64 times the unstretched half-size. The short axis is rebuilt so
///   `cross(out_right, out_up)` retains the camera-facing winding used by the CPU quad and all
///   hardware particle shaders.
/// @param ps Emitter providing the velocity-stretch factor.
/// @param particle Finite particle state whose size and velocity define the axes.
/// @param camera_right Normalized camera right vector.
/// @param camera_up Normalized camera up vector.
/// @param camera_forward Normalized camera forward vector.
/// @param out_right Receives the three-component right half-axis in render space.
/// @param out_up Receives the three-component up/long half-axis in render space.
static void particles3d_billboard_half_axes(const rt_particles3d *ps,
                                            const vgfx3d_particle_t *particle,
                                            const float camera_right[3],
                                            const float camera_up[3],
                                            const float camera_forward[3],
                                            float out_right[3],
                                            float out_up[3]) {
    float axis_r[3];
    float axis_u[3];
    float half_size;
    float half_length;
    if (!ps || !particle || !camera_right || !camera_up || !camera_forward || !out_right || !out_up)
        return;

    half_size = particle->size * 0.5f;
    half_length = half_size;
    memcpy(axis_r, camera_right, sizeof(axis_r));
    memcpy(axis_u, camera_up, sizeof(axis_u));
    if (ps->stretch_k > 0.0) {
        float velocity_x = (float)particle->vel[0];
        float velocity_y = (float)particle->vel[1];
        float velocity_z = (float)particle->vel[2];
        float forward_velocity = velocity_x * camera_forward[0] + velocity_y * camera_forward[1] +
                                 velocity_z * camera_forward[2];
        float projected_x = velocity_x - camera_forward[0] * forward_velocity;
        float projected_y = velocity_y - camera_forward[1] * forward_velocity;
        float projected_z = velocity_z - camera_forward[2] * forward_velocity;
        float projected_length = sqrtf(projected_x * projected_x + projected_y * projected_y +
                                       projected_z * projected_z);
        float speed =
            sqrtf(velocity_x * velocity_x + velocity_y * velocity_y + velocity_z * velocity_z);
        if (isfinite(projected_length) && projected_length > 1e-4f && isfinite(speed)) {
            axis_u[0] = projected_x / projected_length;
            axis_u[1] = projected_y / projected_length;
            axis_u[2] = projected_z / projected_length;
            axis_r[0] = axis_u[1] * camera_forward[2] - axis_u[2] * camera_forward[1];
            axis_r[1] = axis_u[2] * camera_forward[0] - axis_u[0] * camera_forward[2];
            axis_r[2] = axis_u[0] * camera_forward[1] - axis_u[1] * camera_forward[0];
            half_length = half_size * (1.0f + (float)ps->stretch_k * speed);
            if (half_length > half_size * 64.0f)
                half_length = half_size * 64.0f;
        }
    }
    for (int axis = 0; axis < 3; axis++) {
        out_right[axis] = axis_r[axis] * half_size;
        out_up[axis] = axis_u[axis] * half_length;
    }
}

/// @brief Encode one sorted particle as a compact retained-unit-quad instance.
/// @details Center is rebased by the Canvas3D frame origin before narrowing to float. Right/up are
///   the exact half axes shared with the software expansion path, and color is copied unchanged so
///   hardware and software draws reconstruct identical corners, winding, UVs, and modulation.
/// @param ps Emitter providing billboard-stretch configuration.
/// @param particle Particle state to encode.
/// @param origin Camera-relative world origin used for this frame.
/// @param camera_right Normalized camera right vector.
/// @param camera_up Normalized camera up vector.
/// @param camera_forward Normalized camera forward vector.
/// @param out_instance Destination compact record.
static void particles3d_write_compact_instance(const rt_particles3d *ps,
                                               const vgfx3d_particle_t *particle,
                                               const double origin[3],
                                               const float camera_right[3],
                                               const float camera_up[3],
                                               const float camera_forward[3],
                                               vgfx3d_particle_instance_t *out_instance) {
    if (!ps || !particle || !origin || !camera_right || !camera_up || !camera_forward ||
        !out_instance)
        return;
    memset(out_instance, 0, sizeof(*out_instance));
    for (int axis = 0; axis < 3; axis++) {
        double center = particles_clamp_abs_or(
            particle->pos[axis] - origin[axis], 0.0, PARTICLES3D_WORLD_ABS_MAX);
        out_instance->center[axis] = (float)center;
    }
    out_instance->center[3] = 1.0f;
    particles3d_billboard_half_axes(ps,
                                    particle,
                                    camera_right,
                                    camera_up,
                                    camera_forward,
                                    out_instance->right,
                                    out_instance->up);
    memcpy(out_instance->color, particle->color, sizeof(out_instance->color));
}

/// @brief Render every live particle as a camera-facing billboard quad.
/// @details Camera right/up define the billboard axes; alpha particles are sorted back-to-front
///   while additive particles retain pool order. Hardware backends receive one compact sorted
///   center/right/up/color record per particle and draw a retained unit quad. Software backends
///   reconstruct the same records into CPU vertices. Ribbon trails deliberately remain a separate
///   CPU-expanded mesh because each ribbon segment has independent endpoint width and alpha.
/// @param o Particles3D handle; invalid handles are ignored.
/// @param canvas3d Canvas3D handle or valid stack view receiving queued draw commands.
/// @param camera Camera3D handle or valid stack view defining view axes and depth order.
void rt_particles3d_draw(void *o, void *canvas3d, void *camera) {
    rt_particles3d *ps = particles3d_checked(o);
    rt_canvas3d *canvas = rt_canvas3d_checked_or_stack(canvas3d);
    rt_camera3d *cam = rt_camera3d_checked_or_stack(camera);
    if (!ps || !canvas || !cam)
        return;
    particles3d_repair_refs(ps);
    if (!ps->particles || ps->max_particles <= 0)
        return;
    if (ps->count < 0)
        ps->count = 0;
    if (ps->count > ps->max_particles)
        ps->count = ps->max_particles;
    if (ps->terminal_count < 0 || ps->terminal_count > ps->max_particles - ps->count)
        ps->terminal_count = 0;
    for (int32_t i = 0; i < ps->count;) {
        if (particle_state_is_finite(&ps->particles[i])) {
            i++;
            continue;
        }
        particles3d_swap_kill(ps, i);
    }
    for (int32_t i = 0; i < ps->terminal_count; i++) {
        int32_t slot = particles3d_terminal_slot(ps, i);
        if (slot < 0 || !particle_state_is_finite(&ps->particles[slot])) {
            ps->terminal_count = 0;
            break;
        }
    }
    int32_t draw_particle_count = ps->count + ps->terminal_count;
    if (draw_particle_count <= 0)
        return;

    double origin[3] = {0.0, 0.0, 0.0};
    (void)rt_canvas3d_get_camera_relative_origin(canvas3d, origin);

    /* Extract camera right and up vectors from view matrix (row-major).
     * Row 0 = right, Row 1 = up (before translation). */
    float right[3] = {(float)particles_clamp_abs_or(cam->view[0], 1.0, PARTICLES3D_PARAM_MAX),
                      (float)particles_clamp_abs_or(cam->view[1], 0.0, PARTICLES3D_PARAM_MAX),
                      (float)particles_clamp_abs_or(cam->view[2], 0.0, PARTICLES3D_PARAM_MAX)};
    float up[3] = {(float)particles_clamp_abs_or(cam->view[4], 0.0, PARTICLES3D_PARAM_MAX),
                   (float)particles_clamp_abs_or(cam->view[5], 1.0, PARTICLES3D_PARAM_MAX),
                   (float)particles_clamp_abs_or(cam->view[6], 0.0, PARTICLES3D_PARAM_MAX)};
    particles3d_normalize3_or(right, 1.0f, 0.0f, 0.0f);
    particles3d_normalize3_or(up, 0.0f, 1.0f, 0.0f);
    float forward[3] = {right[1] * up[2] - right[2] * up[1],
                        right[2] * up[0] - right[0] * up[2],
                        right[0] * up[1] - right[1] * up[0]};
    particles3d_normalize3_or(forward, 0.0f, 0.0f, 1.0f);

    particle3d_sort_key *sort_keys = NULL;

    /* Sort particles back-to-front for alpha blend (skip for additive or a single quad). */
    if (!ps->additive_blend && draw_particle_count > 1) {
        if (!particles3d_ensure_sort_keys(ps, draw_particle_count))
            return;
        sort_keys = (particle3d_sort_key *)ps->sort_keys;
        if (sort_keys) {
            for (int32_t i = 0; i < draw_particle_count; i++) {
                vgfx3d_particle_t *p = particles3d_draw_particle_at(ps, i);
                if (!p)
                    return;
                double eye_x = particles_clamp_abs_or(cam->eye[0], 0.0, PARTICLES3D_WORLD_ABS_MAX);
                double eye_y = particles_clamp_abs_or(cam->eye[1], 0.0, PARTICLES3D_WORLD_ABS_MAX);
                double eye_z = particles_clamp_abs_or(cam->eye[2], 0.0, PARTICLES3D_WORLD_ABS_MAX);
                double dx =
                    particles_clamp_abs_or(p->pos[0] - eye_x, 0.0, PARTICLES3D_WORLD_ABS_MAX);
                double dy =
                    particles_clamp_abs_or(p->pos[1] - eye_y, 0.0, PARTICLES3D_WORLD_ABS_MAX);
                double dz =
                    particles_clamp_abs_or(p->pos[2] - eye_z, 0.0, PARTICLES3D_WORLD_ABS_MAX);
                sort_keys[i].index = i;
                sort_keys[i].view_depth =
                    dx * (double)forward[0] + dy * (double)forward[1] + dz * (double)forward[2];
                if (!isfinite(sort_keys[i].view_depth))
                    sort_keys[i].view_depth = 0.0;
            }
            particles3d_sort_keys_back_to_front(
                sort_keys, (particle3d_sort_key *)ps->sort_scratch, draw_particle_count);
        }
    }

    /* Trail ribbons remain CPU-expanded on every backend. Hardware backends encode the particle
     * billboards separately below, while software keeps billboards and trails in one CPU mesh. */
    uint32_t trail_quads = 0;
    if (ps->trail_pos && ps->trail_segments > 1) {
        for (int32_t i = 0; i < ps->count; i++) {
            if (ps->trail_len[i] > 1)
                trail_quads += (uint32_t)(ps->trail_len[i] - 1);
        }
    }
    int compact_particles = rt_canvas3d_supports_particle_instancing(canvas3d);
    if (compact_particles && !particles3d_ensure_instance_scratch(ps, draw_particle_count))
        compact_particles = 0;
    uint64_t cpu_quad_count64 = (uint64_t)trail_quads;
    if (!compact_particles)
        cpu_quad_count64 += (uint64_t)draw_particle_count;
    if (cpu_quad_count64 > UINT32_MAX / 6u ||
        cpu_quad_count64 > SIZE_MAX / (4u * sizeof(vgfx3d_vertex_t)) ||
        cpu_quad_count64 > SIZE_MAX / (6u * sizeof(uint32_t))) {
        rt_trap("Particles3D.Draw: particle buffer allocation overflow");
        return;
    }
    uint32_t cpu_quad_count = (uint32_t)cpu_quad_count64;
    uint32_t vert_count = cpu_quad_count * 4u;
    uint32_t idx_count = cpu_quad_count * 6u;
    vgfx3d_vertex_t *verts = NULL;
    uint32_t *indices = NULL;
    void *mat = NULL;
    int canvas_owned_storage = 0;
    if (!particles3d_acquire_draw_storage(
            ps, canvas, vert_count, idx_count, &verts, &indices, &mat, &canvas_owned_storage)) {
        return;
    }
    if (!mat) {
        if (canvas_owned_storage) {
            free(verts);
            free(indices);
        }
        return;
    }

    rt_material3d_set_texture(mat, ps->texture);
    rt_material3d_set_alpha(mat, 1.0);
    rt_material3d_set_alpha_mode(mat, RT_MATERIAL3D_ALPHA_MODE_BLEND);
    ((rt_material3d *)mat)->additive_blend = ps->additive_blend ? 1 : 0;
    ((rt_material3d *)mat)->soft_fade = ps->softness;

    if (compact_particles) {
        for (int32_t i = 0; i < draw_particle_count; i++) {
            int32_t particle_index = sort_keys ? sort_keys[i].index : i;
            vgfx3d_particle_t *particle = particles3d_draw_particle_at(ps, particle_index);
            if (!particle)
                return;
            particles3d_write_compact_instance(
                ps, particle, origin, right, up, forward, &ps->instance_scratch[i]);
        }
        if (!rt_canvas3d_queue_particle_batch(
                canvas3d, mat, ps->instance_scratch, draw_particle_count))
            return;
    } else {
        for (int32_t i = 0; i < draw_particle_count; i++) {
            int32_t particle_index = sort_keys ? sort_keys[i].index : i;
            vgfx3d_particle_t *particle = particles3d_draw_particle_at(ps, particle_index);
            vgfx3d_particle_instance_t billboard;
            uint32_t base = (uint32_t)i * 4u;
            if (!particle)
                return;
            particles3d_write_compact_instance(
                ps, particle, origin, right, up, forward, &billboard);

            /* v0 = bottom-left, v1 = bottom-right, v2 = top-right, v3 = top-left. */
            for (int vi = 0; vi < 4; vi++) {
                float right_sign = (vi == 1 || vi == 2) ? 1.0f : -1.0f;
                float up_sign = (vi == 2 || vi == 3) ? 1.0f : -1.0f;
                vgfx3d_vertex_t *vertex = &verts[base + (uint32_t)vi];
                for (int axis = 0; axis < 3; axis++) {
                    vertex->pos[axis] = billboard.center[axis] +
                                        billboard.right[axis] * right_sign +
                                        billboard.up[axis] * up_sign;
                    vertex->normal[axis] = forward[axis];
                }
                memcpy(vertex->color, billboard.color, sizeof(vertex->color));
            }
            verts[base + 0].uv[0] = 0.0f;
            verts[base + 0].uv[1] = 1.0f;
            verts[base + 1].uv[0] = 1.0f;
            verts[base + 1].uv[1] = 1.0f;
            verts[base + 2].uv[0] = 1.0f;
            verts[base + 2].uv[1] = 0.0f;
            verts[base + 3].uv[0] = 0.0f;
            verts[base + 3].uv[1] = 0.0f;
            for (int vi = 0; vi < 4; vi++)
                particles3d_finalize_draw_vertex(&verts[base + (uint32_t)vi]);

            indices[i * 6 + 0] = base + 0;
            indices[i * 6 + 1] = base + 1;
            indices[i * 6 + 2] = base + 2;
            indices[i * 6 + 3] = base + 0;
            indices[i * 6 + 4] = base + 2;
            indices[i * 6 + 5] = base + 3;
        }
    }
    sort_keys = NULL;

    if (cpu_quad_count == 0)
        return;

    /* Ribbon trails: one camera-facing quad per stored segment pair, appended
     * after the billboards. Width tapers and alpha fades toward the tail. */
    if (trail_quads > 0) {
        uint32_t cursor = compact_particles ? 0u : (uint32_t)draw_particle_count;
        for (int32_t i = 0; i < ps->count; i++) {
            vgfx3d_particle_t *p = &ps->particles[i];
            int16_t len = ps->trail_len[i];
            if (len < 2)
                continue;
            size_t stride = (size_t)ps->trail_segments * 3u;
            const float *ring = &ps->trail_pos[(size_t)i * stride];
            int16_t head = ps->trail_head[i];
            /* Walk newest -> oldest: k = 0 is the most recent control point. */
            for (int16_t k = 0; k + 1 < len && cursor < cpu_quad_count; k++) {
                int16_t i0 =
                    (int16_t)((head - 1 - k + 2 * ps->trail_segments) % ps->trail_segments);
                int16_t i1 =
                    (int16_t)((head - 2 - k + 2 * ps->trail_segments) % ps->trail_segments);
                const float *p0 = &ring[(size_t)i0 * 3u];
                const float *p1 = &ring[(size_t)i1 * 3u];
                float dirx = p1[0] - p0[0];
                float diry = p1[1] - p0[1];
                float dirz = p1[2] - p0[2];
                float side_x = forward[1] * dirz - forward[2] * diry;
                float side_y = forward[2] * dirx - forward[0] * dirz;
                float side_z = forward[0] * diry - forward[1] * dirx;
                float slen = sqrtf(side_x * side_x + side_y * side_y + side_z * side_z);
                if (!isfinite(slen) || slen < 1e-6f)
                    continue;
                side_x /= slen;
                side_y /= slen;
                side_z /= slen;
                float t0 = (float)k / (float)(ps->trail_segments);
                float t1 = (float)(k + 1) / (float)(ps->trail_segments);
                float w0 = p->size * 0.5f * (1.0f - t0 * 0.9f);
                float w1 = p->size * 0.5f * (1.0f - t1 * 0.9f);
                float a0 = p->color[3] * (1.0f - t0);
                float a1 = p->color[3] * (1.0f - t1);
                uint32_t base = cursor * 4;
                for (int vi = 0; vi < 4; vi++) {
                    const float *sp = (vi == 0 || vi == 3) ? p0 : p1;
                    float wgt = (vi == 0 || vi == 3) ? w0 : w1;
                    float sgn = (vi == 0 || vi == 1) ? -1.0f : 1.0f;
                    vgfx3d_vertex_t *v = &verts[base + vi];
                    v->pos[0] = (float)((double)sp[0] - origin[0]) + side_x * wgt * sgn;
                    v->pos[1] = (float)((double)sp[1] - origin[1]) + side_y * wgt * sgn;
                    v->pos[2] = (float)((double)sp[2] - origin[2]) + side_z * wgt * sgn;
                    v->normal[0] = forward[0];
                    v->normal[1] = forward[1];
                    v->normal[2] = forward[2];
                    v->color[0] = p->color[0];
                    v->color[1] = p->color[1];
                    v->color[2] = p->color[2];
                    v->color[3] = (vi == 0 || vi == 3) ? a0 : a1;
                    v->uv[0] = (vi == 0 || vi == 3) ? 0.0f : 1.0f;
                    v->uv[1] = (vi == 0 || vi == 1) ? 1.0f : 0.0f;
                    particles3d_finalize_draw_vertex(v);
                }
                indices[cursor * 6 + 0] = base + 0;
                indices[cursor * 6 + 1] = base + 1;
                indices[cursor * 6 + 2] = base + 2;
                indices[cursor * 6 + 3] = base + 0;
                indices[cursor * 6 + 4] = base + 2;
                indices[cursor * 6 + 5] = base + 3;
                cursor++;
            }
        }
        /* Degenerate any unused reserved quads (culled zero-length segments). */
        while (cursor < cpu_quad_count) {
            uint32_t base = cursor * 4;
            for (int vi = 0; vi < 4; vi++)
                particles3d_write_degenerate_vertex(&verts[base + vi], forward);
            for (int vi = 0; vi < 6; vi++)
                indices[cursor * 6 + vi] = base;
            cursor++;
        }
    }

    /* Create a temporary mesh and submit via the normal draw pipeline.
     * Use unlit material with particle alpha for the draw command. */
    rt_mesh3d tmp_mesh;
    memset(&tmp_mesh, 0, sizeof(tmp_mesh));
    tmp_mesh.vertices = verts;
    tmp_mesh.positions64 = NULL;
    tmp_mesh.vertex_count = vert_count;
    tmp_mesh.vertex_capacity = vert_count;
    tmp_mesh.indices = indices;
    tmp_mesh.index_count = idx_count;
    tmp_mesh.index_capacity = idx_count;
    tmp_mesh.resident = 1;

    if (canvas_owned_storage) {
        int verts_tracked = rt_canvas3d_add_temp_buffer(canvas3d, verts);
        int indices_tracked = rt_canvas3d_add_temp_buffer(canvas3d, indices);
        if (!verts_tracked || !indices_tracked) {
            if (verts_tracked) {
                rt_canvas3d_remove_temp_buffer(canvas3d, verts);
                free(verts);
            }
            if (indices_tracked) {
                rt_canvas3d_remove_temp_buffer(canvas3d, indices);
                free(indices);
            }
            if (!verts_tracked)
                free(verts);
            if (!indices_tracked)
                free(indices);
            if (rt_obj_release_check0(mat))
                rt_obj_free(mat);
            return;
        }
    }

    double model[16];
    particles3d_origin_model_matrix(origin, model);
    rt_canvas3d_draw_mesh_matrix(canvas3d, &tmp_mesh, model, mat);
    if (canvas_owned_storage && rt_obj_release_check0(mat))
        rt_obj_free(mat);
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
