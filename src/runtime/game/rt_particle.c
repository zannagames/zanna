//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_particle.c
/// @file
/// @brief Implements pooled particle simulation, immutable snapshots, and
///        batched Canvas/Pixels rendering.
//
// Purpose: CPU particle emitter for Zanna games. Spawns, simulates, and renders
//   disc-shaped particles with configurable lifetime, speed, angle range,
//   gravity, size, per-particle color, optional fade-out, and optional shrink.
//   Designed for effects like dust, sparks, smoke, blood splatter, and
//   explosions at scales up to RT_PARTICLE_MAX active particles per emitter.
//
// Key invariants:
//   - Each emitter owns a fixed-size particle array (calloc'd at creation,
//     freed on destroy). Capacity is clamped to RT_PARTICLE_MAX.
//   - Emission is rate-based (particles per frame) accumulated with a fractional
//     accumulator, so fractional rates like 0.5 work correctly (one particle
//     every two frames). rt_particle_emitter_burst() ignores the accumulator
//     and fires a fixed count immediately.
//   - Velocity is initialized in polar form (speed, angle in degrees measured
//     counter-clockwise from +X) and converted to Cartesian on spawn. Note that
//     Y velocity is negated because screen Y increases downward.
//   - Per-particle RNG uses a 64-bit LCG (Knuth multiplier) seeded from the
//     active runtime RNG. RANDOMIZE therefore controls emitter reproducibility
//     without leaking object addresses into generated sequences.
//   - Color format is 0xAARRGGBB (alpha in high byte). Tagged Color.RGBA values
//     preserve explicit alpha=0, while legacy 0x00RRGGBB colors remain opaque.
//   - rt_particle_emitter_draw_to_pixels() renders directly to a Pixels canvas
//     via rt_pixels_draw_disc(), avoiding the per-particle dispatch overhead of
//     calling rt_particle_emitter_get() from Zia for every particle.
//
// Ownership/Lifetime:
//   - The emitter struct is GC-managed (rt_obj_new_i64). The particle array is
//     malloc'd separately and freed by a GC finalizer.
//   - rt_particle_emitter_destroy() releases the emitter handle; when the last
//     reference goes away the finalizer reclaims the particle pool.
//
// Links: src/runtime/game/rt_particle.h (public API),
//        src/runtime/graphics/rt_pixels.h (rt_pixels_draw_disc for batch draw),
//        docs/zannalib/game.md (Particle section)
//
//===----------------------------------------------------------------------===//

#include "rt_particle.h"
#include "rt_graphics.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_pixels.h"
#include "rt_random.h"
#include "rt_trap.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/// @brief Tag bit indicating that a packed color's zero alpha is intentional.
#define RT_PARTICLE_COLOR_EXPLICIT_ALPHA_FLAG (INT64_C(1) << 56)

/// @brief One reusable simulated particle record.
/// @details Inactive records remain allocated in the emitter's fixed pool and
///          are overwritten when reused.
struct particle {
    double x, y;       ///< Position.
    double vx, vy;     ///< Velocity.
    double size;       ///< Current size.
    double start_size; ///< Initial size.
    int64_t life;      ///< Remaining frames.
    int64_t max_life;  ///< Total lifetime.
    int64_t color;     ///< Base color.
    int8_t active;     ///< 1 if alive.
};

/// @brief Private configuration, random state, and fixed particle pool.
struct rt_particle_emitter_impl {
    struct particle *particles; ///< Particle array.
    int64_t max_particles;      ///< Maximum particles.
    int64_t active_count;       ///< Number of active particles.
    int64_t emit_cursor;        ///< Round-robin hint for the next free-slot search.

    // Emitter position
    double x, y;

    // Emission settings
    double rate;             ///< Particles per frame.
    double rate_accumulator; ///< Fractional particle accumulator.
    int8_t emitting;         ///< 1 if currently emitting.

    // Particle settings
    int64_t min_life, max_life;
    double min_speed, max_speed;
    double min_angle, max_angle;
    double gx, gy;
    int64_t color;
    double min_size, max_size;
    int8_t fade_out;
    int8_t shrink;

    // Random state (simple LCG)
    uint64_t rand_state;
};

/// @brief Immutable runtime object returned for safe particle queries.
struct rt_particle_snapshot_impl {
    double x;
    double y;
    double size;
    int64_t color;
};

/// @brief Validate and cast an opaque ParticleEmitter handle.
/// @param emitter Candidate handle; `NULL` is accepted.
/// @param api Trap message for a non-null class mismatch.
/// @return @p emitter when valid, or `NULL` for null or invalid input.
/// @details A class mismatch raises a runtime trap before returning `NULL`.
static struct rt_particle_emitter_impl *checked_emitter(rt_particle_emitter emitter,
                                                        const char *api) {
    if (!emitter)
        return NULL;
    if (rt_obj_class_id(emitter) != RT_PARTICLE_EMITTER_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return emitter;
}

/// @brief Validate and cast an opaque ParticleSnapshot handle.
/// @param snapshot Candidate handle; `NULL` is accepted.
/// @param api Trap message for a non-null class mismatch.
/// @return @p snapshot when valid, or `NULL` for null or invalid input.
/// @details A class mismatch raises a runtime trap before returning `NULL`.
static struct rt_particle_snapshot_impl *checked_snapshot(rt_particle_snapshot snapshot,
                                                          const char *api) {
    if (!snapshot)
        return NULL;
    if (rt_obj_class_id(snapshot) != RT_PARTICLE_SNAPSHOT_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return snapshot;
}

/// @brief Allocate an immutable particle-value snapshot.
/// @param x Particle X position.
/// @param y Particle Y position.
/// @param size Rendered particle diameter.
/// @param color Effective packed `0xAARRGGBB` color.
/// @return A new runtime-managed snapshot reference, or `NULL` on allocation
///         failure.
rt_particle_snapshot rt_particle_snapshot_new(double x, double y, double size, int64_t color) {
    struct rt_particle_snapshot_impl *snapshot = (struct rt_particle_snapshot_impl *)rt_obj_new_i64(
        RT_PARTICLE_SNAPSHOT_CLASS_ID, (int64_t)sizeof(struct rt_particle_snapshot_impl));
    if (!snapshot)
        return NULL;
    snapshot->x = x;
    snapshot->y = y;
    snapshot->size = size;
    snapshot->color = color;
    return snapshot;
}

/// @brief Read a snapshot's X position.
/// @param snapshot Snapshot to query.
/// @return Stored X coordinate, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_snapshot_x(rt_particle_snapshot snapshot) {
    snapshot =
        checked_snapshot(snapshot, "ParticleSnapshot.X: expected Zanna.Game.ParticleSnapshot");
    return snapshot ? snapshot->x : 0.0;
}

/// @brief Read a snapshot's Y position.
/// @param snapshot Snapshot to query.
/// @return Stored Y coordinate, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_snapshot_y(rt_particle_snapshot snapshot) {
    snapshot =
        checked_snapshot(snapshot, "ParticleSnapshot.Y: expected Zanna.Game.ParticleSnapshot");
    return snapshot ? snapshot->y : 0.0;
}

/// @brief Read a snapshot's rendered diameter.
/// @param snapshot Snapshot to query.
/// @return Stored size, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_snapshot_size(rt_particle_snapshot snapshot) {
    snapshot =
        checked_snapshot(snapshot, "ParticleSnapshot.Size: expected Zanna.Game.ParticleSnapshot");
    return snapshot ? snapshot->size : 0.0;
}

/// @brief Read a snapshot's effective packed color.
/// @param snapshot Snapshot to query.
/// @return Stored `0xAARRGGBB` value, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_particle_snapshot_color(rt_particle_snapshot snapshot) {
    snapshot =
        checked_snapshot(snapshot, "ParticleSnapshot.Color: expected Zanna.Game.ParticleSnapshot");
    return snapshot ? snapshot->color : 0;
}

/// @brief Replace a non-finite floating-point value with a fallback.
/// @param value Candidate value.
/// @param fallback Value returned for NaN or either infinity.
/// @return @p value when finite; otherwise @p fallback.
static double particle_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Add two signed integers without overflowing.
/// @param a First addend.
/// @param b Second addend.
/// @return Exact sum when representable, otherwise the nearest int64 endpoint.
static int64_t particle_saturating_add(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Convert a finite in-range double to int64 by truncation.
/// @param value Candidate floating-point value.
/// @param out Receives the result on success; may be `NULL`.
/// @return `1` on success, or `0` when @p out is null, @p value is non-finite,
///         or the conversion would be outside the open int64 endpoint bounds.
/// @details On failure, the pointed-to output is left unchanged.
static int8_t particle_double_to_i64(double value, int64_t *out) {
    if (!out || !isfinite(value))
        return 0;
    if (value >= (double)INT64_MAX || value <= (double)INT64_MIN)
        return 0;
    *out = (int64_t)value;
    return 1;
}

/// @brief Convert a particle diameter to a bounded integer draw radius.
/// @param size Particle diameter in pixels.
/// @return Truncated `size / 2` clamped to `[1, 1,000,000]`; non-finite input
///         also returns one.
static int64_t particle_radius_from_size(double size) {
    if (!isfinite(size))
        return 1;
    double radius = size * 0.5;
    if (radius < 1.0)
        return 1;
    if (radius > 1000000.0)
        return 1000000;
    return (int64_t)radius;
}

/// @brief Advance the emitter-local LCG and produce a uniform fraction.
/// @param e Emitter whose random state is advanced.
/// @return A pseudo-random value in `[0, 1)`.
static double rand_double(struct rt_particle_emitter_impl *e) {
    e->rand_state = e->rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(e->rand_state >> 33) / (double)(1ULL << 31);
}

/// @brief Choose a floating-point value from a sanitized inclusive range.
/// @param e Emitter providing random state.
/// @param min Requested lower endpoint; non-finite values become zero.
/// @param max Requested upper endpoint; non-finite or smaller values become
///        the sanitized minimum.
/// @return A pseudo-random value from the resulting range.
static double rand_range(struct rt_particle_emitter_impl *e, double min, double max) {
    if (!isfinite(min))
        min = 0.0;
    if (!isfinite(max))
        max = min;
    if (max < min)
        max = min;
    return min + rand_double(e) * (max - min);
}

/// @brief Choose an integer uniformly from an inclusive signed range.
/// @param e Emitter providing random state.
/// @param min Inclusive lower endpoint.
/// @param max Inclusive upper endpoint.
/// @return A pseudo-random integer in `[min, max]`, or @p min when
///         `min >= max`.
/// @details Unsigned offset arithmetic permits ranges spanning the full signed
///          domain without signed overflow.
static int64_t rand_range_i64(struct rt_particle_emitter_impl *e, int64_t min, int64_t max) {
    if (min >= max)
        return min;
    // Work in unsigned offset space so the full int64 range is representable
    // without signed overflow (negative `min`, or min/max spanning the entire
    // range). `range` = max-min is well-defined for min < max, and the result
    // min+offset is mathematically within [min, max], so it always fits int64.
    uint64_t range = (uint64_t)max - (uint64_t)min;
    uint64_t offset = (uint64_t)(rand_double(e) * ((long double)range + 1.0L));
    if (offset > range)
        offset = range;
    return (int64_t)((uint64_t)min + offset);
}

/// @brief Resolve a particle's draw color, applying legacy-opaque handling for
///        0x00RRGGBB colors and life-ratio alpha fade when fade_out is set.
/// @param emitter Emitter supplying the fade-out setting.
/// @param p Active particle whose base color and lifetime are read.
/// @return Packed 0xAARRGGBB color with the effective alpha in the high byte.
static int64_t particle_effective_color(const struct rt_particle_emitter_impl *emitter,
                                        const struct particle *p) {
    int64_t color = p->color;
    int64_t argb = color & 0xFFFFFFFF;
    int64_t alpha = (argb >> 24) & 0xFF;
    if (((uint64_t)color & (uint64_t)RT_PARTICLE_COLOR_EXPLICIT_ALPHA_FLAG) == 0 && alpha == 0)
        alpha = 255; // Legacy 0x00RRGGBB colors are treated as opaque.
    if (emitter->fade_out && p->max_life > 0) {
        const double life_ratio = (double)p->life / (double)p->max_life;
        alpha = (int64_t)(alpha * life_ratio);
    }
    return (argb & 0x00FFFFFF) | (alpha << 24);
}

/// @brief Convert ARGB packing to the Pixels buffer's RGBA packing.
/// @param color Packed `0xAARRGGBB` value; higher bits are ignored.
/// @return Equivalent packed `0xRRGGBBAA` value.
static uint32_t particle_argb_to_rgba(int64_t color) {
    uint32_t argb = (uint32_t)(color & 0xFFFFFFFFu);
    uint32_t a = (argb >> 24) & 0xFFu;
    uint32_t r = (argb >> 16) & 0xFFu;
    uint32_t g = (argb >> 8) & 0xFFu;
    uint32_t b = argb & 0xFFu;
    return (r << 24) | (g << 16) | (b << 8) | a;
}

/// @brief Porter-Duff "source over destination" alpha compositing of two
///        0xRRGGBBAA colors, with rounded straight-alpha output.
/// @param dst Existing destination pixel.
/// @param src Source pixel placed over @p dst.
/// @return Composited packed `0xRRGGBBAA` pixel.
/// @details Fast paths for fully transparent/opaque source; otherwise blends
///          in premultiplied space and un-premultiplies the result.
static uint32_t particle_alpha_over_rgba(uint32_t dst, uint32_t src) {
    uint32_t sr = (src >> 24) & 0xFFu;
    uint32_t sg = (src >> 16) & 0xFFu;
    uint32_t sb = (src >> 8) & 0xFFu;
    uint32_t sa = src & 0xFFu;
    uint32_t dr = (dst >> 24) & 0xFFu;
    uint32_t dg = (dst >> 16) & 0xFFu;
    uint32_t db = (dst >> 8) & 0xFFu;
    uint32_t da = dst & 0xFFu;

    if (sa == 0)
        return dst;
    if (sa == 255)
        return src;

    uint32_t inv = 255u - sa;
    uint32_t oa = sa + (da * inv + 127u) / 255u;
    if (oa == 0)
        return 0;

    uint32_t r_pm = sr * sa + (dr * da * inv + 127u) / 255u;
    uint32_t g_pm = sg * sa + (dg * da * inv + 127u) / 255u;
    uint32_t b_pm = sb * sa + (db * da * inv + 127u) / 255u;
    uint32_t r = (r_pm + oa / 2u) / oa;
    uint32_t g = (g_pm + oa / 2u) / oa;
    uint32_t b = (b_pm + oa / 2u) / oa;
    if (r > 255u)
        r = 255u;
    if (g > 255u)
        g = 255u;
    if (b > 255u)
        b = 255u;
    return (r << 24) | (g << 16) | (b << 8) | oa;
}

/// @brief Test point membership in a disc without signed integer overflow.
/// @param x Point X coordinate.
/// @param y Point Y coordinate.
/// @param cx Disc center X coordinate.
/// @param cy Disc center Y coordinate.
/// @param radius Disc radius.
/// @return `1` when the point is inside or on the boundary, otherwise `0`.
static int8_t particle_point_in_disc_i64(
    int64_t x, int64_t y, int64_t cx, int64_t cy, int64_t radius) {
    long double dx = (long double)x - (long double)cx;
    long double dy = (long double)y - (long double)cy;
    long double rr = (long double)radius * (long double)radius;
    return dx * dx + dy * dy <= rr;
}

/// @brief Draw a filled, alpha-blended disc of @p radius at (cx, cy) into a
///        Pixels buffer, compositing each covered pixel via
///        particle_alpha_over_rgba (clipped to the buffer bounds).
/// @param pixels Valid Pixels buffer to mutate.
/// @param cx Disc center X coordinate.
/// @param cy Disc center Y coordinate.
/// @param radius Radius in pixels, capped at one million.
/// @param color Packed `0xAARRGGBB` source color.
/// @details Fully transparent colors draw nothing. Coverage is clipped to the
///          buffer and composited pixel by pixel in RGBA byte order.
static void rt_particle_draw_disc_blended(
    void *pixels, int64_t cx, int64_t cy, int64_t radius, int64_t color) {
    int64_t alpha = (color >> 24) & 0xFF;
    uint32_t src = particle_argb_to_rgba(color);

    if (radius > 1000000)
        radius = 1000000;
    if (alpha <= 0)
        return;
    int64_t width = rt_pixels_width(pixels);
    int64_t height = rt_pixels_height(pixels);
    int64_t min_x = particle_saturating_add(cx, -radius);
    int64_t max_x = particle_saturating_add(cx, radius);
    int64_t min_y = particle_saturating_add(cy, -radius);
    int64_t max_y = particle_saturating_add(cy, radius);
    if (min_x < 0)
        min_x = 0;
    if (min_y < 0)
        min_y = 0;
    if (max_x >= width)
        max_x = width - 1;
    if (max_y >= height)
        max_y = height - 1;

    for (int64_t y = min_y; y <= max_y; ++y) {
        for (int64_t x = min_x; x <= max_x; ++x) {
            if (!particle_point_in_disc_i64(x, y, cx, cy, radius))
                continue;
            uint32_t dst = (uint32_t)rt_pixels_get(pixels, x, y);
            rt_pixels_set(pixels, x, y, (int64_t)particle_alpha_over_rgba(dst, src));
        }
    }
}

/// @brief Release an emitter's separately allocated particle pool.
/// @param obj Emitter object supplied by the runtime finalizer system.
/// @details Null objects and already-cleared arrays are safe no-ops.
static void particle_emitter_finalizer(void *obj) {
    struct rt_particle_emitter_impl *e = (struct rt_particle_emitter_impl *)obj;
    if (e && e->particles) {
        free(e->particles);
        e->particles = NULL;
    }
}

/// @brief Create a particle emitter with a fixed, reusable particle pool.
/// @details Allocates the particle pool array up front. Particles are spawned
///          via start() (continuous emission at the configured rate) or burst()
///          (one-shot). Each particle has independent position, velocity, lifetime,
///          size, and color. Defaults are a stopped emitter at the origin,
///          rate 1, lifetime 30–60 ticks, speed 1–3, angle 0–360 degrees, no
///          gravity, opaque white color, size 2–4, fade enabled, and shrink
///          disabled. The local LCG is seeded from the active runtime RNG.
/// @param max_particles Maximum simultaneous particles, clamped to
///        `[1, RT_PARTICLE_MAX]`.
/// @return A new runtime-managed emitter reference, or `NULL` if the object or
///         particle pool cannot be allocated.
rt_particle_emitter rt_particle_emitter_new(int64_t max_particles) {
    if (max_particles < 1)
        max_particles = 1;
    if (max_particles > RT_PARTICLE_MAX)
        max_particles = RT_PARTICLE_MAX;

    struct rt_particle_emitter_impl *e = (struct rt_particle_emitter_impl *)rt_obj_new_i64(
        RT_PARTICLE_EMITTER_CLASS_ID, (int64_t)sizeof(struct rt_particle_emitter_impl));
    if (!e)
        return NULL;

    e->particles = calloc((size_t)max_particles, sizeof(struct particle));
    if (!e->particles) {
        if (rt_obj_release_check0(e))
            rt_obj_free(e);
        return NULL;
    }

    e->max_particles = max_particles;
    e->active_count = 0;
    e->emit_cursor = 0;

    e->x = 0.0;
    e->y = 0.0;
    e->rate = 1.0;
    e->rate_accumulator = 0.0;
    e->emitting = 0;

    e->min_life = 30;
    e->max_life = 60;
    e->min_speed = 1.0;
    e->max_speed = 3.0;
    e->min_angle = 0.0;
    e->max_angle = 360.0;
    e->gx = 0.0;
    e->gy = 0.0;
    e->color = 0xFFFFFFFF; // White, full alpha
    e->min_size = 2.0;
    e->max_size = 4.0;
    e->fade_out = 1;
    e->shrink = 0;

    // Seed random from the active runtime RNG; avoid zero because non-zero
    // seeds give better early LCG distribution.
    e->rand_state = ((uint64_t)rt_rand_range(0, LLONG_MAX) << 1) ^
                    (uint64_t)rt_rand_range(0, LLONG_MAX) ^ UINT64_C(0x5DEECE66D);
    if (e->rand_state == 0)
        e->rand_state = UINT64_C(0x5DEECE66D);

    rt_obj_set_finalizer(e, particle_emitter_finalizer);
    return e;
}

/// @brief Release one runtime reference to a particle emitter.
/// @param emitter Handle to release; `NULL` is a no-op.
/// @details The particle-pool finalizer runs when the last reference is freed.
///          A non-null handle of another runtime class raises a trap.
void rt_particle_emitter_destroy(rt_particle_emitter emitter) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.Destroy: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    if (rt_obj_release_check0(emitter))
        rt_obj_free(emitter);
}

/// @brief Set the origin copied into subsequently spawned particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param x New world-space X coordinate. NaN or infinity preserves the
///        previous X coordinate.
/// @param y New world-space Y coordinate. NaN or infinity preserves the
///        previous Y coordinate.
/// @details Existing particles are not moved. A non-null wrong-class handle
///          raises a runtime trap.
void rt_particle_emitter_set_position(rt_particle_emitter emitter, double x, double y) {
    emitter = checked_emitter(emitter,
                              "ParticleEmitter.SetPosition: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    emitter->x = particle_finite_or(x, emitter->x);
    emitter->y = particle_finite_or(y, emitter->y);
}

/// @brief Read the emitter's current spawn-origin X coordinate.
/// @param emitter Emitter to query.
/// @return Stored X coordinate, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_emitter_x(rt_particle_emitter emitter) {
    emitter = checked_emitter(emitter, "ParticleEmitter.X: expected Zanna.Game.ParticleEmitter");
    return emitter ? emitter->x : 0.0;
}

/// @brief Read the emitter's current spawn-origin Y coordinate.
/// @param emitter Emitter to query.
/// @return Stored Y coordinate, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_emitter_y(rt_particle_emitter emitter) {
    emitter = checked_emitter(emitter, "ParticleEmitter.Y: expected Zanna.Game.ParticleEmitter");
    return emitter ? emitter->y : 0.0;
}

/// @brief Set the continuous emission rate in particles per update tick.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param rate Requested rate. Negative and non-finite values become zero;
///        values above RT_PARTICLE_MAX are capped.
/// @details Fractional particles accumulate across update calls while the
///          emitter is started and has capacity. A non-null wrong-class
///          handle raises a runtime trap.
void rt_particle_emitter_set_rate(rt_particle_emitter emitter, double rate) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.SetRate: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    if (!isfinite(rate) || rate < 0.0)
        rate = 0.0;
    if (rate > (double)RT_PARTICLE_MAX)
        rate = (double)RT_PARTICLE_MAX;
    emitter->rate = rate;
}

/// @brief Read the configured continuous emission rate.
/// @param emitter Emitter to query.
/// @return Particles per update tick, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_emitter_rate(rt_particle_emitter emitter) {
    emitter = checked_emitter(emitter, "ParticleEmitter.Rate: expected Zanna.Game.ParticleEmitter");
    return emitter ? emitter->rate : 0.0;
}

/// @brief Set the inclusive lifetime range for newly spawned particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param min_frames Requested minimum update ticks, clamped to at least one.
/// @param max_frames Requested maximum update ticks, raised to the sanitized
///        minimum when smaller.
/// @details Each new particle chooses an integer uniformly from this range.
///          Existing particles retain their lifetimes. A non-null wrong-class
///          handle raises a runtime trap.
void rt_particle_emitter_set_lifetime(rt_particle_emitter emitter,
                                      int64_t min_frames,
                                      int64_t max_frames) {
    emitter = checked_emitter(emitter,
                              "ParticleEmitter.SetLifetime: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    if (min_frames < 1)
        min_frames = 1;
    if (max_frames < min_frames)
        max_frames = min_frames;
    emitter->min_life = min_frames;
    emitter->max_life = max_frames;
}

/// @brief Configure launch speeds and angles for newly spawned particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param min_speed Minimum nonnegative units per tick; negative or non-finite
///        input becomes zero.
/// @param max_speed Maximum units per tick; non-finite or smaller input becomes
///        the sanitized minimum.
/// @param min_angle Minimum angle in degrees counter-clockwise from positive X;
///        non-finite input becomes zero.
/// @param max_angle Maximum angle in degrees; non-finite input becomes the
///        sanitized minimum.
/// @details Angles are not normalized. Spawn converts polar velocity to screen
///          coordinates by negating sine because positive screen Y is down.
///          Existing particles retain their velocities. A non-null
///          wrong-class handle raises a runtime trap.
void rt_particle_emitter_set_velocity(rt_particle_emitter emitter,
                                      double min_speed,
                                      double max_speed,
                                      double min_angle,
                                      double max_angle) {
    emitter = checked_emitter(emitter,
                              "ParticleEmitter.SetVelocity: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    if (!isfinite(min_speed) || min_speed < 0.0)
        min_speed = 0.0;
    if (!isfinite(max_speed) || max_speed < min_speed)
        max_speed = min_speed;
    if (!isfinite(min_angle))
        min_angle = 0.0;
    if (!isfinite(max_angle))
        max_angle = min_angle;
    emitter->min_speed = min_speed;
    emitter->max_speed = max_speed;
    emitter->min_angle = min_angle;
    emitter->max_angle = max_angle;
}

/// @brief Set the acceleration applied to every active particle per tick.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param gx Horizontal acceleration; NaN or infinity becomes zero.
/// @param gy Vertical acceleration; NaN or infinity becomes zero. Positive
///        values accelerate toward increasing screen Y.
/// @details A non-null wrong-class handle raises a runtime trap.
void rt_particle_emitter_set_gravity(rt_particle_emitter emitter, double gx, double gy) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.SetGravity: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    emitter->gx = particle_finite_or(gx, 0.0);
    emitter->gy = particle_finite_or(gy, 0.0);
}

/// @brief Set the base color copied into newly spawned particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param color Packed `0xAARRGGBB` value or tagged runtime color value.
/// @details Legacy `0x00RRGGBB` values render as opaque. Tagged runtime colors
///          can preserve an explicit zero alpha. Existing particles retain
///          their colors. A non-null wrong-class handle raises a trap.
void rt_particle_emitter_set_color(rt_particle_emitter emitter, int64_t color) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.SetColor: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    emitter->color = color;
}

/// @brief Configure the initial diameter range for newly spawned particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param min_size Requested minimum diameter. Non-finite or values below 0.1
///        become 0.1; values above the final maximum are lowered to it.
/// @param max_size Requested maximum diameter. Non-finite or smaller values
///        become the sanitized minimum, then values above 2,000,000 are capped.
/// @details Each spawn samples the resulting range. Rendering converts
///          diameter to a radius capped at 1,000,000 pixels. Existing
///          particles retain their initial sizes. A non-null wrong-class
///          handle raises a runtime trap.
void rt_particle_emitter_set_size(rt_particle_emitter emitter, double min_size, double max_size) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.SetSize: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    if (!isfinite(min_size) || min_size < 0.1)
        min_size = 0.1;
    if (!isfinite(max_size) || max_size < min_size)
        max_size = min_size;
    if (max_size > 2000000.0)
        max_size = 2000000.0;
    if (min_size > max_size)
        min_size = max_size;
    emitter->min_size = min_size;
    emitter->max_size = max_size;
}

/// @brief Enable or disable lifetime-proportional alpha fading.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param fade_out Zero disables fading; any nonzero value enables it.
/// @details Fading is evaluated when particles are queried or drawn and
///          therefore affects existing particles immediately. A non-null
///          wrong-class handle raises a runtime trap.
void rt_particle_emitter_set_fade_out(rt_particle_emitter emitter, int8_t fade_out) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.SetFadeOut: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    emitter->fade_out = fade_out ? 1 : 0;
}

/// @brief Enable or disable lifetime-proportional particle shrinking.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param shrink Zero disables shrinking; any nonzero value enables it.
/// @details Shrinking updates particle size during simulation and affects
///          existing particles on the next tick. A non-null wrong-class
///          handle raises a runtime trap.
void rt_particle_emitter_set_shrink(rt_particle_emitter emitter, int8_t shrink) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.SetShrink: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    emitter->shrink = shrink ? 1 : 0;
}

/// @brief Enable continuous emission at the configured rate.
/// @param emitter Emitter to start; `NULL` is a no-op.
/// @details Spawning begins on a subsequent update. Existing particles are
///          unaffected. A non-null wrong-class handle raises a runtime trap.
void rt_particle_emitter_start(rt_particle_emitter emitter) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.Start: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    emitter->emitting = 1;
}

/// @brief Disable continuous emission without clearing live particles.
/// @param emitter Emitter to stop; `NULL` is a no-op.
/// @details Existing particles continue to simulate and expire. A non-null
///          wrong-class handle raises a runtime trap.
void rt_particle_emitter_stop(rt_particle_emitter emitter) {
    emitter = checked_emitter(emitter, "ParticleEmitter.Stop: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    emitter->emitting = 0;
}

/// @brief Test whether continuous emission is enabled.
/// @param emitter Emitter to query.
/// @return `1` when started, or `0` when stopped or null.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_particle_emitter_is_emitting(rt_particle_emitter emitter) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.IsEmitting: expected Zanna.Game.ParticleEmitter");
    return emitter ? emitter->emitting : 0;
}

/// @brief Test whether lifetime-proportional alpha fading is enabled.
/// @param emitter Emitter to query.
/// @return `1` when enabled, or `0` when disabled or null.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_particle_emitter_fade_out(rt_particle_emitter emitter) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.FadeOut: expected Zanna.Game.ParticleEmitter");
    return emitter ? emitter->fade_out : 0;
}

/// @brief Test whether lifetime-proportional size shrinking is enabled.
/// @param emitter Emitter to query.
/// @return `1` when enabled, or `0` when disabled or null.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_particle_emitter_shrink(rt_particle_emitter emitter) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.Shrink: expected Zanna.Game.ParticleEmitter");
    return emitter ? emitter->shrink : 0;
}

/// @brief Read the base color assigned to new particles.
/// @param emitter Emitter to query.
/// @return Stored packed or tagged color value, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_particle_emitter_color(rt_particle_emitter emitter) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.Color: expected Zanna.Game.ParticleEmitter");
    return emitter ? emitter->color : 0;
}

/// @brief Spawn one particle into the next inactive pool record.
/// @param e Valid emitter with an allocated particle array.
/// @details A round-robin search begins at `emit_cursor`. On success the
///          particle receives the current origin and randomized configured
///          properties. A full pool causes a silent no-op.
static void emit_one(struct rt_particle_emitter_impl *e) {
    // Resume the free-slot search from the last emit position (round-robin) so
    // bursts and high emission rates don't rescan the filled prefix from index 0
    // each call — amortized O(1) when free slots exist instead of O(n).
    for (int64_t n = 0; n < e->max_particles; n++) {
        int64_t i = e->emit_cursor + n;
        if (i >= e->max_particles)
            i -= e->max_particles;
        if (!e->particles[i].active) {
            struct particle *p = &e->particles[i];

            p->x = e->x;
            p->y = e->y;

            double speed = rand_range(e, e->min_speed, e->max_speed);
            double angle = rand_range(e, e->min_angle, e->max_angle) * M_PI / 180.0;
            p->vx = cos(angle) * speed;
            p->vy = -sin(angle) * speed; // Negative because Y typically increases downward

            p->life = rand_range_i64(e, e->min_life, e->max_life);
            p->max_life = p->life;
            p->size = rand_range(e, e->min_size, e->max_size);
            p->start_size = p->size;
            p->color = e->color;
            p->active = 1;

            e->active_count++;
            e->emit_cursor = (i + 1 >= e->max_particles) ? 0 : i + 1;
            return;
        }
    }
}

/// @brief Immediately emit a requested number of particles.
/// @param emitter Emitter whose current configuration and origin are used.
/// @param count Requested particle count; values below one are ignored.
/// @details Emission stops when @p count particles have spawned or the pool is
///          full. The fractional continuous-emission accumulator and emitting
///          state are unchanged. A null emitter is a no-op; a non-null
///          wrong-class handle raises a runtime trap.
void rt_particle_emitter_burst(rt_particle_emitter emitter, int64_t count) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.Burst: expected Zanna.Game.ParticleEmitter");
    if (!emitter || count < 1)
        return;
    for (int64_t i = 0; i < count && emitter->active_count < emitter->max_particles; i++) {
        emit_one(emitter);
    }
}

/// @brief Advance particle simulation and continuous emission by one tick.
/// @param emitter Emitter to update; `NULL` is a no-op.
/// @details Each previously active particle first moves by its velocity, then
///          receives gravity, optional shrinking, and one tick of aging.
///          Non-finite simulation state and expired particles are removed.
///          Continuous particles spawn afterward so they retain their full
///          initial lifetime for the next tick. Fractional rate is accumulated,
///          but a pool that began the tick full resets the accumulator and does
///          not emit that tick even if aging frees slots. A non-null
///          wrong-class handle raises a runtime trap.
void rt_particle_emitter_update(rt_particle_emitter emitter) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.Update: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;

    int8_t was_full = emitter->active_count >= emitter->max_particles;

    // Update existing particles
    emitter->active_count = 0;
    for (int64_t i = 0; i < emitter->max_particles; i++) {
        struct particle *p = &emitter->particles[i];
        if (!p->active)
            continue;

        // Apply velocity
        p->x += p->vx;
        p->y += p->vy;

        // Apply gravity
        p->vx += emitter->gx;
        p->vy += emitter->gy;
        if (!isfinite(p->x) || !isfinite(p->y) || !isfinite(p->vx) || !isfinite(p->vy) ||
            !isfinite(p->size)) {
            p->active = 0;
            continue;
        }

        // Shrink if enabled
        if (emitter->shrink && p->max_life > 0) {
            double life_ratio = (double)p->life / (double)p->max_life;
            p->size = p->start_size * life_ratio;
        }

        // Decrease lifetime
        p->life--;
        if (p->life <= 0) {
            p->active = 0;
        } else {
            emitter->active_count++;
        }
    }

    // Emit after aging so newly spawned particles live for a full update tick.
    if (emitter->emitting) {
        if (!isfinite(emitter->rate_accumulator) || was_full)
            emitter->rate_accumulator = 0.0;
        if (!was_full) {
            emitter->rate_accumulator += emitter->rate;
            if (!isfinite(emitter->rate_accumulator))
                emitter->rate_accumulator = 0.0;
            while (emitter->rate_accumulator >= 1.0 &&
                   emitter->active_count < emitter->max_particles) {
                emit_one(emitter);
                emitter->rate_accumulator -= 1.0;
            }
            if (emitter->active_count >= emitter->max_particles)
                emitter->rate_accumulator = 0.0;
        }
    }
}

/// @brief Count the emitter's currently active particles.
/// @param emitter Emitter to query.
/// @return Live particle count, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_particle_emitter_count(rt_particle_emitter emitter) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.Count: expected Zanna.Game.ParticleEmitter");
    return emitter ? emitter->active_count : 0;
}

/// @brief Deactivate every particle and discard fractional emission credit.
/// @param emitter Emitter to clear; `NULL` is a no-op.
/// @details Configuration and started/stopped state are preserved. A started
///          emitter may spawn again on the next update. A non-null wrong-class
///          handle raises a runtime trap.
void rt_particle_emitter_clear(rt_particle_emitter emitter) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.Clear: expected Zanna.Game.ParticleEmitter");
    if (!emitter)
        return;
    for (int64_t i = 0; i < emitter->max_particles; i++) {
        emitter->particles[i].active = 0;
    }
    emitter->active_count = 0;
    emitter->rate_accumulator = 0.0;
}

/// @brief Read render values for the Nth active particle in pool-index order.
/// @param emitter Emitter to query.
/// @param index Zero-based active-particle ordinal, not a stable pool index.
/// @param out_x Optional destination for world-space X.
/// @param out_y Optional destination for world-space Y.
/// @param out_size Optional destination for current diameter.
/// @param out_color Optional destination for effective packed `0xAARRGGBB`
///        color after legacy-alpha and fade-out handling.
/// @return `1` when @p index exists, even if all outputs are null; otherwise
///         `0`.
/// @details Outputs are written only on success and null output pointers are
///          allowed. Burst, clear, and update can change ordinal-to-particle
///          mapping. A null emitter or negative/out-of-range ordinal fails; a
///          non-null wrong-class handle raises a runtime trap. Searching is
///          O(pool capacity).
int8_t rt_particle_emitter_get(rt_particle_emitter emitter,
                               int64_t index,
                               double *out_x,
                               double *out_y,
                               double *out_size,
                               int64_t *out_color) {
    emitter = checked_emitter(emitter, "ParticleEmitter.ParticleAt: expected Zanna.Game.ParticleEmitter");
    if (!emitter || index < 0)
        return 0;

    // Find the Nth active particle
    int64_t found = 0;
    for (int64_t i = 0; i < emitter->max_particles; i++) {
        struct particle *p = &emitter->particles[i];
        if (!p->active)
            continue;

        if (found == index) {
            if (out_x)
                *out_x = p->x;
            if (out_y)
                *out_y = p->y;
            if (out_size)
                *out_size = p->size;

            if (out_color) {
                *out_color = particle_effective_color(emitter, p);
            }
            return 1;
        }
        found++;
    }
    return 0;
}

/// @brief Return an optional immutable snapshot of one active particle.
/// @param emitter Emitter to query.
/// @param index Zero-based active-particle ordinal in pool-index order.
/// @return `Option<ParticleSnapshot>` containing copied position, current
///         diameter, and effective color, or `None` for invalid input or
///         allocation failure.
/// @details The returned snapshot does not alias emitter storage and remains
///          unchanged by later simulation. A non-null wrong-class emitter
///          raises a runtime trap through rt_particle_emitter_get().
void *rt_particle_emitter_particle_at(rt_particle_emitter emitter, int64_t index) {
    double x = 0.0;
    double y = 0.0;
    double size = 0.0;
    int64_t color = 0;
    if (!rt_particle_emitter_get(emitter, index, &x, &y, &size, &color))
        return rt_option_none();

    rt_particle_snapshot snapshot = rt_particle_snapshot_new(x, y, size, color);
    if (!snapshot)
        return rt_option_none();
    void *option = rt_option_some(snapshot);
    if (rt_obj_release_check0(snapshot))
        rt_obj_free(snapshot);
    return option;
}

/// @brief Alpha-blend every drawable particle into a Pixels buffer.
/// @param emitter Emitter to render.
/// @param pixels Destination object, which must have RT_PIXELS_CLASS_ID.
/// @param offset_x Signed X offset added with saturation after truncating the
///        particle's world position.
/// @param offset_y Signed Y offset added with saturation after truncating the
///        particle's world position.
/// @return Number of active particles with convertible finite positions that
///         were processed, or zero for invalid input.
/// @details Each particle becomes a clipped filled disc. Diameter is converted
///          to a radius in `[1, 1,000,000]`, and source-over blending preserves
///          destination alpha. The count includes fully transparent particles,
///          although they modify no pixels. A non-null wrong-class emitter
///          traps; a null or wrong-class Pixels destination simply returns zero.
int64_t rt_particle_emitter_draw_to_pixels(rt_particle_emitter emitter,
                                           void *pixels,
                                           int64_t offset_x,
                                           int64_t offset_y) {
    emitter = checked_emitter(emitter,
                              "ParticleEmitter.DrawToPixels: expected Zanna.Game.ParticleEmitter");
    if (!emitter || !pixels || rt_obj_class_id(pixels) != RT_PIXELS_CLASS_ID)
        return 0;

    int64_t drawn = 0;

    for (int64_t i = 0; i < emitter->max_particles; i++) {
        struct particle *p = &emitter->particles[i];
        if (!p->active)
            continue;

        int64_t base_x = 0;
        int64_t base_y = 0;
        if (!particle_double_to_i64(p->x, &base_x) || !particle_double_to_i64(p->y, &base_y))
            continue;
        int64_t px = particle_saturating_add(base_x, offset_x);
        int64_t py = particle_saturating_add(base_y, offset_y);
        int64_t radius = particle_radius_from_size(p->size);

        rt_particle_draw_disc_blended(pixels, px, py, radius, particle_effective_color(emitter, p));
        drawn++;
    }

    return drawn;
}

/// @brief Draw active particles to a Canvas without positional offsets.
/// @param emitter Emitter to render.
/// @param canvas Destination Canvas handle.
/// @return The value returned by rt_particle_emitter_draw_at() with zero
///         offsets.
int64_t rt_particle_emitter_draw(rt_particle_emitter emitter, void *canvas) {
    return rt_particle_emitter_draw_at(emitter, canvas, 0, 0);
}

/// @brief Draw active particles as alpha-blended Canvas discs with offsets.
/// @param emitter Emitter to render.
/// @param canvas Destination Canvas handle.
/// @param offset_x Signed X offset added with saturation after truncating the
///        particle's world position.
/// @param offset_y Signed Y offset added with saturation after truncating the
///        particle's world position.
/// @return Number of active particles with convertible finite positions passed
///         to the Canvas renderer, or zero for invalid input.
/// @details Diameter becomes a radius in `[1, 1,000,000]`. The effective ARGB
///          alpha is supplied separately from the low-24-bit Canvas color. The
///          count includes zero-alpha particles. A non-null wrong-class emitter
///          traps; a null or invalid Canvas simply returns zero.
int64_t rt_particle_emitter_draw_at(rt_particle_emitter emitter,
                                    void *canvas,
                                    int64_t offset_x,
                                    int64_t offset_y) {
    emitter =
        checked_emitter(emitter, "ParticleEmitter.DrawAt: expected Zanna.Game.ParticleEmitter");
    if (!emitter || !canvas || !rt_canvas_is_handle(canvas))
        return 0;

    int64_t drawn = 0;

    for (int64_t i = 0; i < emitter->max_particles; i++) {
        struct particle *p = &emitter->particles[i];
        if (!p->active)
            continue;

        int64_t color = particle_effective_color(emitter, p);
        int64_t alpha = (color >> 24) & 0xFF;
        int64_t canvas_color = color & 0x00FFFFFF;
        int64_t base_x = 0;
        int64_t base_y = 0;
        if (!particle_double_to_i64(p->x, &base_x) || !particle_double_to_i64(p->y, &base_y))
            continue;
        int64_t px = particle_saturating_add(base_x, offset_x);
        int64_t py = particle_saturating_add(base_y, offset_y);
        int64_t radius = particle_radius_from_size(p->size);

        rt_canvas_disc_alpha(canvas, px, py, radius, canvas_color, alpha);
        drawn++;
    }

    return drawn;
}
