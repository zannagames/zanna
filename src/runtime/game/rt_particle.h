//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_particle.h
/// @file
/// @brief Declares pooled particle simulation, immutable snapshots, and
///        batched software rendering.
//
// Purpose: Particle emitter for visual effects (explosions, sparks, smoke) managing a pool of
// particles with configurable lifetime, velocity, gravity, color, size, fade, and shrink.
//
// Key invariants:
//   - Maximum particle count is RT_PARTICLE_MAX (1024) per emitter.
//   - rt_particle_emitter_update must be called once per frame.
//   - Particle query indices are active-particle ordinals, not stable pool IDs,
//     and may change after burst, clear, or update operations.
//   - Emission rate is fractional particles-per-frame; sub-integer amounts accumulate.
//
// Ownership/Lifetime:
//   - Emitters and snapshots are runtime reference-counted objects. Destroying
//     an emitter releases one reference and its finalizer owns the pool array.
//   - Output pointers written by rt_particle_emitter_get are not retained.
//
// Links: src/runtime/game/rt_particle.c (implementation),
// src/runtime/game/rt_screenfx.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID used to validate ParticleEmitter handles.
#define RT_PARTICLE_EMITTER_CLASS_ID INT64_C(-0x510204)

/// @brief Runtime class ID used to validate ParticleSnapshot handles.
#define RT_PARTICLE_SNAPSHOT_CLASS_ID INT64_C(-0x510205)

/// @brief Maximum simultaneous particle capacity accepted by the constructor.
#define RT_PARTICLE_MAX 1024

/// @brief Opaque handle to a runtime reference-counted ParticleEmitter.
typedef struct rt_particle_emitter_impl *rt_particle_emitter;

/// @brief Opaque immutable copy of one particle's render values.
typedef struct rt_particle_snapshot_impl *rt_particle_snapshot;

/// @brief Allocate an emitter with a fixed reusable particle pool.
/// @param max_particles Requested simultaneous capacity, clamped to
///        `[1, RT_PARTICLE_MAX]`.
/// @return A new ParticleEmitter reference, or `NULL` if allocation fails.
/// @details The emitter begins stopped at `(0,0)` with rate 1, lifetime 30–60,
///          speed 1–3, full-circle angles, zero gravity, opaque white color,
///          size 2–4, fade-out enabled, and shrinking disabled. Its local
///          random generator is seeded from the active runtime RNG.
rt_particle_emitter rt_particle_emitter_new(int64_t max_particles);

/// @brief Release one runtime reference to an emitter.
/// @param emitter Handle to release; `NULL` is a no-op.
/// @details The particle pool is reclaimed when the final reference is freed.
///          A non-null wrong-class handle raises a runtime trap.
void rt_particle_emitter_destroy(rt_particle_emitter emitter);

/// @brief Set the world-space origin copied into future particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param x New X coordinate; non-finite input preserves the current X.
/// @param y New Y coordinate; non-finite input preserves the current Y.
/// @details Existing particles are not moved. A non-null wrong-class handle
///          raises a runtime trap.
void rt_particle_emitter_set_position(rt_particle_emitter emitter, double x, double y);

/// @brief Read the emitter's spawn-origin X coordinate.
/// @param emitter Emitter to query.
/// @return Stored X coordinate, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_emitter_x(rt_particle_emitter emitter);

/// @brief Read the emitter's spawn-origin Y coordinate.
/// @param emitter Emitter to query.
/// @return Stored Y coordinate, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_emitter_y(rt_particle_emitter emitter);

/// @brief Set the continuous emission rate.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param rate Particles spawned per update. Negative and non-finite values
///        become zero; values above RT_PARTICLE_MAX are capped.
/// @details Fractional amounts accumulate while started and capacity remains.
///          A non-null wrong-class handle raises a runtime trap.
void rt_particle_emitter_set_rate(rt_particle_emitter emitter, double rate);

/// @brief Read the configured continuous emission rate.
/// @param emitter Emitter to query.
/// @return Particles per update tick, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_emitter_rate(rt_particle_emitter emitter);

/// @brief Configure the inclusive lifetime range for future particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param min_frames Minimum update ticks, clamped to at least one.
/// @param max_frames Maximum ticks, raised to the sanitized minimum if smaller.
/// @details Each spawn samples an integer uniformly from the resulting range.
///          Existing lifetimes are unchanged. A non-null wrong-class handle
///          raises a runtime trap.
void rt_particle_emitter_set_lifetime(rt_particle_emitter emitter,
                                      int64_t min_frames,
                                      int64_t max_frames);

/// @brief Configure launch speed and angle ranges for future particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param min_speed Minimum nonnegative units per tick; negative or non-finite
///        values become zero.
/// @param max_speed Maximum units per tick; non-finite or smaller values become
///        the sanitized minimum.
/// @param min_angle Minimum degrees counter-clockwise from positive X;
///        non-finite input becomes zero.
/// @param max_angle Maximum degrees; non-finite input becomes the sanitized
///        minimum.
/// @details Angles are not normalized. Positive 90 degrees launches upward
///          because spawn negates sine for downward-positive screen Y.
///          Existing velocities are unchanged. A non-null wrong-class handle
///          raises a runtime trap.
void rt_particle_emitter_set_velocity(rt_particle_emitter emitter,
                                      double min_speed,
                                      double max_speed,
                                      double min_angle,
                                      double max_angle);

/// @brief Set per-tick acceleration for all active particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param gx Horizontal acceleration; non-finite input becomes zero.
/// @param gy Vertical acceleration; non-finite input becomes zero. Positive
///        values accelerate toward increasing screen Y.
/// @details A non-null wrong-class handle raises a runtime trap.
void rt_particle_emitter_set_gravity(rt_particle_emitter emitter, double gx, double gy);

/// @brief Set the base color copied into future particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param color Packed `0xAARRGGBB` value or tagged runtime color value.
/// @details Legacy `0x00RRGGBB` colors render as opaque, while tagged runtime
///          colors preserve explicit alpha zero. Fade-out may reduce alpha.
///          Existing colors are unchanged. A non-null wrong-class handle traps.
void rt_particle_emitter_set_color(rt_particle_emitter emitter, int64_t color);

/// @brief Configure the initial diameter range for future particles.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param min_size Requested minimum; non-finite or values below 0.1 become
///        0.1, then values above the final maximum are lowered to it.
/// @param max_size Requested maximum; non-finite or smaller values become the
///        sanitized minimum, then values above 2,000,000 are capped.
/// @details Spawn samples this range. Rendering halves diameter to obtain a
///          radius capped at 1,000,000. Existing particle sizes are unchanged.
///          A non-null wrong-class handle raises a runtime trap.
void rt_particle_emitter_set_size(rt_particle_emitter emitter, double min_size, double max_size);

/// @brief Enable or disable lifetime-proportional alpha fading.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param fade_out Zero disables fading; any nonzero value enables it.
/// @details The setting is evaluated at query/draw time and affects existing
///          particles immediately. A non-null wrong-class handle traps.
void rt_particle_emitter_set_fade_out(rt_particle_emitter emitter, int8_t fade_out);

/// @brief Enable or disable lifetime-proportional diameter shrinking.
/// @param emitter Emitter to modify; `NULL` is a no-op.
/// @param shrink Zero disables shrinking; any nonzero value enables it.
/// @details Shrinking affects existing particles during subsequent updates. A
///          non-null wrong-class handle raises a runtime trap.
void rt_particle_emitter_set_shrink(rt_particle_emitter emitter, int8_t shrink);

/// @brief Enable continuous particle emission.
/// @param emitter Emitter to start; `NULL` is a no-op.
/// @details Spawning begins on a subsequent update and continues at the
///          configured rate until stopped. Existing particles are unaffected.
///          A non-null wrong-class handle raises a runtime trap.
void rt_particle_emitter_start(rt_particle_emitter emitter);

/// @brief Disable continuous emission without clearing live particles.
/// @param emitter Emitter to stop; `NULL` is a no-op.
/// @details Existing particles continue to simulate and expire. A non-null
///          wrong-class handle raises a runtime trap.
void rt_particle_emitter_stop(rt_particle_emitter emitter);

/// @brief Test whether continuous emission is enabled.
/// @param emitter Emitter to query.
/// @return `1` when started, or `0` when stopped or null.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_particle_emitter_is_emitting(rt_particle_emitter emitter);

/// @brief Test whether alpha fading is enabled.
/// @param emitter Emitter to query.
/// @return `1` when enabled, or `0` when disabled or null.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_particle_emitter_fade_out(rt_particle_emitter emitter);

/// @brief Test whether diameter shrinking is enabled.
/// @param emitter Emitter to query.
/// @return `1` when enabled, or `0` when disabled or null.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_particle_emitter_shrink(rt_particle_emitter emitter);

/// @brief Read the base color assigned to future particles.
/// @param emitter Emitter to query.
/// @return Stored packed or tagged color value, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_particle_emitter_color(rt_particle_emitter emitter);

/// @brief Immediately spawn particles from the current origin.
/// @param emitter Emitter whose current configuration is used.
/// @param count Requested number; values below one are ignored.
/// @details Spawning stops at @p count or pool capacity. The fractional
///          continuous-rate accumulator and emitting state are unchanged. A
///          null emitter is a no-op; a non-null wrong-class handle traps.
void rt_particle_emitter_burst(rt_particle_emitter emitter, int64_t count);

/// @brief Advances all live particles by one simulation frame.
/// @param emitter Emitter to update; `NULL` is a no-op.
/// @details Existing particles move, receive gravity and optional shrinking,
///          then age; expired or non-finite particles are removed. Continuous
///          particles spawn afterward with their full lifetime. Fractional
///          rate accumulates while capacity remains. An emitter that began the
///          tick full resets its accumulator and emits nothing that tick even
///          if aging frees slots. A non-null wrong-class handle traps.
void rt_particle_emitter_update(rt_particle_emitter emitter);

/// @brief Count currently active particles.
/// @param emitter Emitter to query.
/// @return Live count in `[0, max_particles]`, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_particle_emitter_count(rt_particle_emitter emitter);

/// @brief Remove every particle and discard fractional emission credit.
/// @param emitter Emitter to clear; `NULL` is a no-op.
/// @details Configuration and started/stopped state are preserved, so a
///          started emitter may spawn on the next update. A non-null
///          wrong-class handle raises a runtime trap.
void rt_particle_emitter_clear(rt_particle_emitter emitter);

/// @brief Read render values for the Nth active particle in pool-index order.
/// @param emitter Emitter to query.
/// @param index Zero-based active-particle ordinal, not a stable slot ID.
/// @param out_x Optional destination for world-space X.
/// @param out_y Optional destination for world-space Y.
/// @param out_size Optional destination for current diameter.
/// @param out_color Optional destination for effective packed `0xAARRGGBB`
///        color after legacy-alpha and fade-out handling.
/// @return `1` if the ordinal exists, even when every output is null; otherwise
///         `0`.
/// @details Failed queries leave non-null outputs unchanged. Burst, clear, and
///          update may alter ordinal mapping. Lookup is O(pool capacity). A
///          non-null wrong-class handle raises a runtime trap.
int8_t rt_particle_emitter_get(rt_particle_emitter emitter,
                               int64_t index,
                               double *out_x,
                               double *out_y,
                               double *out_size,
                               int64_t *out_color);

/// @brief Copy one active particle's render values into an optional snapshot.
/// @param emitter Emitter to query.
/// @param index Zero-based active-particle ordinal in pool-index order.
/// @return `Option<ParticleSnapshot>` containing a new immutable copy, or
///         `None` for an invalid ordinal, null emitter, or allocation failure.
/// @details The snapshot does not alias emitter storage. A non-null
///          wrong-class emitter raises a runtime trap.
void *rt_particle_emitter_particle_at(rt_particle_emitter emitter, int64_t index);

/// @brief Allocate an immutable snapshot from explicit render values.
/// @param x Particle X position.
/// @param y Particle Y position.
/// @param size Particle diameter.
/// @param color Effective packed `0xAARRGGBB` color.
/// @return A new runtime-managed snapshot reference, or `NULL` on allocation
///         failure.
rt_particle_snapshot rt_particle_snapshot_new(double x, double y, double size, int64_t color);

/// @brief Read a snapshot's X position.
/// @param snapshot Snapshot to query.
/// @return Stored X coordinate, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_snapshot_x(rt_particle_snapshot snapshot);

/// @brief Read a snapshot's Y position.
/// @param snapshot Snapshot to query.
/// @return Stored Y coordinate, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_snapshot_y(rt_particle_snapshot snapshot);

/// @brief Read a snapshot's rendered diameter.
/// @param snapshot Snapshot to query.
/// @return Stored size, or `0.0` for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
double rt_particle_snapshot_size(rt_particle_snapshot snapshot);

/// @brief Read a snapshot's effective packed color.
/// @param snapshot Snapshot to query.
/// @return Stored `0xAARRGGBB` value, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_particle_snapshot_color(rt_particle_snapshot snapshot);

/// @brief Batch-render all live particles directly into a Pixels buffer.
/// @param emitter Emitter to render.
/// @param pixels Destination object with RT_PIXELS_CLASS_ID.
/// @param offset_x Signed X offset added with saturation after truncating each
///        particle position.
/// @param offset_y Signed Y offset added with saturation after truncating each
///        particle position.
/// @return Number of active particles with convertible finite positions that
///         were processed, or zero for invalid input.
/// @details Each particle is rasterized as a clipped filled disc and
///          source-over blended pixel by pixel in the Pixels RGBA byte order.
///          Diameter becomes a radius in `[1, 1,000,000]`. The returned count
///          includes fully transparent particles even though they change no
///          pixels. A non-null wrong-class emitter traps; a null or wrong-class
///          Pixels object returns zero.
int64_t rt_particle_emitter_draw_to_pixels(rt_particle_emitter emitter,
                                           void *pixels,
                                           int64_t offset_x,
                                           int64_t offset_y);

/// @brief Batch-render live particles onto a Canvas without offsets.
/// @param emitter Emitter to render.
/// @param canvas Destination Canvas handle.
/// @return The result of rt_particle_emitter_draw_at() with zero offsets.
/// @details A non-null wrong-class emitter traps. A null or invalid Canvas
///          causes zero to be returned.
int64_t rt_particle_emitter_draw(rt_particle_emitter emitter, void *canvas);

/// @brief Batch-render all live particles onto a Canvas with position offsets.
/// @param emitter Emitter to render.
/// @param canvas Destination Canvas handle.
/// @param offset_x Signed X offset added with saturation after truncating each
///        particle position.
/// @param offset_y Signed Y offset added with saturation after truncating each
///        particle position.
/// @return Number of active particles with convertible finite positions passed
///         to the Canvas renderer, or zero for invalid input.
/// @details Each particle is passed to rt_canvas_disc_alpha() using its low
///          24-bit color and effective alpha. Diameter becomes a radius in
///          `[1, 1,000,000]`. The count includes zero-alpha particles. A
///          non-null wrong-class emitter traps; a null or invalid Canvas
///          returns zero.
int64_t rt_particle_emitter_draw_at(rt_particle_emitter emitter,
                                    void *canvas,
                                    int64_t offset_x,
                                    int64_t offset_y);

#ifdef __cplusplus
}
#endif
