//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_timeofday3d.h
// Purpose: Public C ABI for the deterministic time-of-day clock driving a sun
//   Light3D, a Sky3D, and reflection-probe re-capture flags.
// Key invariants:
//   - Advance(dt) is the only clock input; no wall-clock reads.
// Ownership/Lifetime:
//   - GC-managed; bound consumers are retained until rebound/finalized.
// Links: rt_timeofday3d.c, misc/plans/thirdpersonupgrade/16-timeofday-weather.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_timeofday3d.h
 * @brief Declares the deterministic TimeOfDay3D clock and environment-driver C ABI.
 *
 * The clock derives a stylized solar direction from authored hours and latitude, drives retained
 * light/sky/probe consumers, and never reads wall-clock time.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a paused clock at solar noon (latitude 35, refresh 2 degrees).
/// @return New GC-managed TimeOfDay3D handle, or NULL after allocation failure.
void *rt_timeofday3d_new(void);

/// @brief Clock hour in [0, 24); wraps.
/// @param tod TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param hours Finite hour value wrapped into `[0, 24)`.
void rt_timeofday3d_set_hours(void *tod, double hours);

/// @brief Return the current wrapped clock hour.
/// @param tod TimeOfDay3D handle; invalid handles trap.
/// @return Hour in `[0, 24)`, or zero when validation fails.
double rt_timeofday3d_get_hours(void *tod);

/// @brief Real seconds per 24h day; 0 pauses the clock (drive Hours manually).
/// @param tod TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param seconds Finite nonnegative simulated-day duration; zero pauses automatic advancement.
void rt_timeofday3d_set_day_length_seconds(void *tod, double seconds);

/// @brief Return the configured duration of a simulated day.
/// @param tod TimeOfDay3D handle; invalid handles trap.
/// @return Nonnegative duration in seconds, or zero when paused or invalid.
double rt_timeofday3d_get_day_length_seconds(void *tod);

/// @brief Latitude tilt applied to the sun arc (-85..85 degrees).
/// @param tod TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param degrees Finite latitude in `[-85, 85]`; out-of-range input is ignored.
void rt_timeofday3d_set_latitude_degrees(void *tod, double degrees);

/// @brief Return the latitude used by the solar-direction model.
/// @param tod TimeOfDay3D handle; invalid handles trap.
/// @return Latitude in degrees, or zero when validation fails.
double rt_timeofday3d_get_latitude_degrees(void *tod);

/// @brief Sun movement (degrees) that triggers sky/probe refresh (throttle).
/// @param tod TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param degrees Threshold greater than 0.1 degrees, clamped to 45.
void rt_timeofday3d_set_refresh_degrees(void *tod, double degrees);

/// @brief Return the angular sky/probe refresh threshold.
/// @param tod TimeOfDay3D handle; invalid handles trap.
/// @return Threshold in degrees, or zero when validation fails.
double rt_timeofday3d_get_refresh_degrees(void *tod);

/// @brief Bind the directional Light3D driven by the sun model.
/// @param tod TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param light Light3D handle to retain, or NULL to clear the binding.
void rt_timeofday3d_set_sun_light(void *tod, void *light);

/// @brief Bind the procedural Sky3D refreshed when the sun crosses the angular threshold.
/// @param tod TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param sky Sky3D handle to retain, or NULL to clear the binding.
void rt_timeofday3d_set_sky(void *tod, void *sky);

/// @brief Bind the ReflectionProbe3D marked dirty when the sun crosses the angular threshold.
/// @param tod TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param probe ReflectionProbe3D handle to retain, or NULL to clear the binding.
void rt_timeofday3d_set_reflection_probe(void *tod, void *probe);

/// @brief Direction TOWARD the sun for the current hour (raw and Vec3 forms).
/// @param tod TimeOfDay3D handle; invalid handles trap.
/// @param out_dir Three-element output receiving the normalized sun direction.
void rt_timeofday3d_get_sun_direction_raw(void *tod, double out_dir[3]);

/// @brief Return the current direction toward the sun as a new Vec3.
/// @param tod TimeOfDay3D handle; invalid handles trap.
/// @return New GC-managed Vec3 direction, or NULL on allocation failure.
void *rt_timeofday3d_get_sun_direction(void *tod);

/// @brief Advance by @p dt seconds and drive bound consumers (canvas may be NULL).
/// @param tod TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param dt Positive finite scaled simulation delta in seconds; whole simulated days are reduced
/// modulo the configured day length before conversion to hours.
/// @param canvas Optional Canvas3D handle that receives procedural-sky refreshes.
void rt_timeofday3d_advance(void *tod, double dt, void *canvas);

#ifdef __cplusplus
}
#endif
