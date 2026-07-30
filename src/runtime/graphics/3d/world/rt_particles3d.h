//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_particles3d.h
// Purpose: 3D particle system with emitter-based spawning, physics (gravity,
//   velocity), lifetime management, size/color/alpha interpolation, and
//   camera-facing billboard rendering with batched draw calls.
//
// Key invariants:
//   - Particle state is pooled; spawning and removal do not allocate.
//   - Hardware billboards use compact instances; software and ribbons use CPU-expanded meshes.
//   - Billboards face the camera using view matrix right/up vectors.
//   - Alpha blend mode sorts particles back-to-front; additive is unordered.
//   - Update executes bounded 60 Hz substeps, carries fractional time, and
//     exposes any safety-budget drop through queryable telemetry.
//   - Expired particles leave the live count immediately; an optional
//     terminal snapshot can render their exact endpoint for one update frame.
//
// Ownership/Lifetime:
//   - The GC-managed emitter owns its particle, trail, sort, draw, and compact-instance scratch.
//   - Texture/material references are retained by the emitter and repaired before use.
//
// Links: plans/3d/17-particle-system.md, rt_canvas3d.h,
//   docs/internals/graphics3d-runtime-hardening-2026-07.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_particles3d.h
 * @brief Declares the GC-managed 3D particle-emitter runtime API.
 *
 * The API configures a fixed-capacity emitter, advances it through bounded fixed simulation
 * steps, and queues camera-facing billboard or ribbon-trail rendering through Canvas3D.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a particle emitter pre-allocated for at most @p max_particles concurrent
/// particles.
/// @param max_particles Fixed pool capacity from 1 through 100000.
/// @return New GC-managed Particles3D handle, or NULL after an invalid request or allocation
/// failure.
void *rt_particles3d_new(int64_t max_particles);

/// @brief Set the emitter origin in world space.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param x World-space x coordinate.
/// @param y World-space y coordinate.
/// @param z World-space z coordinate.
void rt_particles3d_set_position(void *obj, double x, double y, double z);

/// @brief Set the base emission direction and a cone spread angle in radians.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param dx Emit-axis x component.
/// @param dy Emit-axis y component.
/// @param dz Emit-axis z component.
/// @param spread Cone half-angle in radians, or degrees when greater than pi and at most 180.
void rt_particles3d_set_direction(void *obj, double dx, double dy, double dz, double spread);

/// @brief Set the per-particle speed range (m/s) — random uniform sample at spawn.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param min_speed First nonnegative speed bound.
/// @param max_speed Second nonnegative speed bound; reversed bounds are reordered.
void rt_particles3d_set_speed(void *obj, double min_speed, double max_speed);

/// @brief Set the per-particle lifetime range in seconds.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param min_life First positive lifetime bound.
/// @param max_life Second positive lifetime bound; reversed bounds are reordered.
void rt_particles3d_set_lifetime(void *obj, double min_life, double max_life);

/// @brief Set particle size at spawn vs death (lerped over lifetime).
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param start_size Nonnegative billboard size at spawn.
/// @param end_size Nonnegative billboard size at expiration.
void rt_particles3d_set_size(void *obj, double start_size, double end_size);

/// @brief Set the world-space gravity acceleration applied to every particle.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param gx World-space x acceleration.
/// @param gy World-space y acceleration.
/// @param gz World-space z acceleration.
void rt_particles3d_set_gravity(void *obj, double gx, double gy, double gz);

/// @brief Set particle color at spawn vs death (lerped over lifetime).
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param start_color Packed `0xRRGGBB` color at spawn.
/// @param end_color Packed `0xRRGGBB` color at expiration.
void rt_particles3d_set_color(void *obj, int64_t start_color, int64_t end_color);

/// @brief Set particle alpha at spawn vs death.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param start_alpha Spawn alpha, clamped to `[0, 1]`.
/// @param end_alpha Expiration alpha, clamped to `[0, 1]`.
void rt_particles3d_set_alpha(void *obj, double start_alpha, double end_alpha);

/// @brief Set the steady-state spawn rate while emitting (particles per second).
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param particles_per_second Requested nonnegative emission rate.
void rt_particles3d_set_rate(void *obj, double particles_per_second);

/// @brief Toggle additive blending (1 = additive, 0 = alpha blend with back-to-front sort).
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param additive Nonzero for order-independent additive blending, zero for sorted alpha blending.
void rt_particles3d_set_additive(void *obj, int8_t additive);
/// @brief Read whether additive blending is enabled (ADR 0227).
/// @param obj Particles3D receiver.
/// @return Nonzero for additive blending, or 0 for invalid handles.
int8_t rt_particles3d_get_additive(void *obj);

/// @brief Velocity-aligned stretching (0 = camera-facing; k scales length by speed).
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param k Nonnegative velocity-to-length scale, clamped to `[0, 8]`.
void rt_particles3d_set_stretch(void *obj, double k);

/// @brief Ribbon trails: seconds of history over 2..16 control points (<= 0 disables).
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param lifetime_sec Seconds of history to retain; nonpositive values disable and free trails.
/// @param segments Control-point count per particle, clamped to `[2, 16]`.
void rt_particles3d_set_trail(void *obj, double lifetime_sec, int64_t segments);

/// @brief Set the geometry-intersection fade distance used by soft particles.
/// @details Zero disables soft fading; backends without soft-particle support ignore this setting.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param distance Nonnegative fade distance in world units.
void rt_particles3d_set_softness(void *obj, double distance);

/// @brief Bind a Pixels texture for billboard rendering (NULL = solid color quads).
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param pixels Pixels handle to retain, or NULL to render untextured quads.
void rt_particles3d_set_texture(void *obj, void *pixels);

/// @brief Set the emitter volume shape to point, sphere, or box.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param shape Shape identifier: 0 for point, 1 for sphere, or 2 for box.
void rt_particles3d_set_emitter_shape(void *obj, int64_t shape);

/// @brief Set the emitter shape's per-axis dimensions (interpretation depends on shape).
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param sx Sphere radius or box x half-extent.
/// @param sy Box y half-extent.
/// @param sz Box z half-extent.
void rt_particles3d_set_emitter_size(void *obj, double sx, double sy, double sz);

/// @brief Start continuous emission at the configured rate.
/// @param obj Particles3D handle; invalid handles are ignored.
void rt_particles3d_start(void *obj);

/// @brief Stop continuous emission (existing particles continue until their lifetime expires).
/// @param obj Particles3D handle; invalid handles are ignored.
void rt_particles3d_stop(void *obj);

/// @brief Spawn @p count particles immediately as a one-shot burst.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param count Requested number of immediate spawns, limited by available pool capacity.
void rt_particles3d_burst(void *obj, int64_t count);

/// @brief Despawn every active particle and reset the emitter timer.
/// @param obj Particles3D handle; invalid handles are ignored.
void rt_particles3d_clear(void *obj);

/// @brief Advance particles through bounded fixed substeps and spawn new ones if emitting.
/// @details Fractional time is carried in `ResidualTime`. Calls execute at most 60 1/60-second
///          steps; excess complete-step time is reported by dropped-time telemetry.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param delta_time Positive finite elapsed time in seconds.
void rt_particles3d_update(void *obj, double delta_time);

/// @brief Render all active particles as camera-facing billboards in a single batched draw.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param canvas3d Canvas3D handle or valid stack view receiving the queued draw.
/// @param camera Camera3D handle or valid stack view defining billboard axes and depth order.
void rt_particles3d_draw(void *obj, void *canvas3d, void *camera);

/// @brief Internal: shift emitter and live particle world positions by `-delta`.
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param dx World-origin x displacement to subtract.
/// @param dy World-origin y displacement to subtract.
/// @param dz World-origin z displacement to subtract.
void rt_particles3d_rebase_origin(void *obj, double dx, double dy, double dz);

/// @brief Internal: copy the emitter world position into @p out (zeroed on invalid handle).
/// @param obj Particles3D handle.
/// @param out Three-element output array; no data is written when NULL.
void rt_particles3d_get_position(void *obj, double out[3]);

/// @brief Number of currently-alive particles.
/// @param obj Particles3D handle.
/// @return Validated live-particle count, or zero for an invalid handle.
int64_t rt_particles3d_get_count(void *obj);

/// @brief True if the emitter is in continuous-emission mode (between Start and Stop).
/// @param obj Particles3D handle.
/// @return 1 while continuous emission is enabled, otherwise 0.
int8_t rt_particles3d_get_emitting(void *obj);

/// @brief Set the live spawn PRNG state (reproducible streams; 0 maps to the fallback seed).
/// @param obj Particles3D handle; invalid handles are ignored.
/// @param seed New spawn PRNG state; zero selects the built-in nonzero fallback.
void rt_particles3d_set_seed(void *obj, int64_t seed);

/// @brief Current spawn PRNG state (checkpointable for replay).
/// @param obj Particles3D handle.
/// @return Current unsigned 32-bit state widened to `int64_t`, or zero for an invalid handle.
int64_t rt_particles3d_get_seed(void *obj);

/// @brief Enable or disable rendering an expired particle's exact endpoint for one update frame.
/// @details Enabled by default for visual compatibility. Expired particles are never included in
///          `Count`; disabling this option immediately discards pending terminal snapshots.
/// @param obj Particles3D handle.
/// @param enabled Nonzero to retain one terminal render snapshot, zero to disable it.
void rt_particles3d_set_render_final_frame(void *obj, int8_t enabled);

/// @brief Return whether one-frame terminal endpoint rendering is enabled.
/// @param obj Particles3D handle.
/// @return 1 when enabled, otherwise 0 (including invalid handles).
int8_t rt_particles3d_get_render_final_frame(void *obj);

/// @brief Return cumulative catch-up time explicitly dropped by bounded Update calls.
/// @param obj Particles3D handle.
/// @return Nonnegative seconds since construction or `ResetDroppedTime`; zero if invalid.
double rt_particles3d_get_dropped_time(void *obj);

/// @brief Return catch-up time dropped by the most recent Update call.
/// @param obj Particles3D handle.
/// @return Nonnegative seconds, or zero when the last update dropped none/handle is invalid.
double rt_particles3d_get_last_dropped_time(void *obj);

/// @brief Return the fractional fixed-step time carried into the next Update.
/// @param obj Particles3D handle.
/// @return Residual seconds in `[0, 1/60)`, or zero for an invalid handle.
double rt_particles3d_get_residual_time(void *obj);

/// @brief Reset cumulative and last-update dropped-time telemetry without changing simulation.
/// @param obj Particles3D handle; invalid handles are ignored.
void rt_particles3d_reset_dropped_time(void *obj);

#ifdef __cplusplus
}
#endif
