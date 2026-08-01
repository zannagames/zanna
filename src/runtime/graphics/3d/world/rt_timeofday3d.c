//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_timeofday3d.c
// Purpose: Time-of-day clock — advances an hours value, computes the sun
//   direction from hour angle + latitude, drives a bound sun Light3D
//   (direction, elevation-keyed color/intensity curve), a bound Sky3D (sun
//   direction + throttled regeneration), and flags a bound ReflectionProbe3D
//   for re-capture when the sun moves past the refresh threshold.
// Key invariants:
//   - Advance(dt) is the only clock input: deterministic under the world's
//     scaled dt, replay-stable, no wall-clock reads.
// Ownership/Lifetime:
//   - GC-managed; bound light/sky/probe are retained until finalized/rebound.
// Links: misc/plans/thirdpersonupgrade/16-timeofday-weather.md, ADR 0090.
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_timeofday3d.c
 * @brief Implements a deterministic in-world clock that drives sun, sky, and reflection state.
 *
 * Time advances only from caller-provided simulation deltas. The derived sun direction updates a
 * retained directional light every call, while expensive sky and reflection refreshes are
 * throttled by angular movement.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_timeofday3d.h"
#include "rt_canvas3d.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_reflectionprobe3d.h"
#include "rt_sky3d.h"
#include "rt_trap.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int32_t rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern void *rt_vec3_new(double x, double y, double z);
extern void rt_light3d_set_direction(void *light, void *direction);
extern void rt_light3d_set_color(void *light, double r, double g, double b);
extern void rt_light3d_set_intensity(void *light, double intensity);
extern void rt_light3d_set_enabled(void *light, int8_t enabled);

typedef struct rt_timeofday3d {
    void *vptr;
    double hours;              /* [0, 24) */
    double day_length_seconds; /* 0 = paused (drive hours manually) */
    double latitude_degrees;
    double refresh_degrees; /* sun movement that triggers sky/probe refresh */
    void *sun_light;        /* retained Light3D, or NULL */
    void *sky;              /* retained Sky3D, or NULL */
    void *probe;            /* retained ReflectionProbe3D, or NULL */
    double last_refresh_dir[3];
    int8_t has_refresh_dir;
} rt_timeofday3d;

/// @brief Release and clear one retained consumer slot without touching a wrong-class pointer.
/// @param slot Address of the retained-object slot.
/// @param class_id Runtime class the slot is allowed to own.
static void timeofday3d_release(void **slot, int64_t class_id) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, class_id)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    rt_g3d_ref_slot_release(slot);
}

/// @brief Restore finite, bounded clock configuration before derived arithmetic.
/// @param[in,out] tod Clock state to repair; null is ignored.
static void timeofday3d_repair_state(rt_timeofday3d *tod) {
    if (!tod)
        return;
    if (!isfinite(tod->hours))
        tod->hours = 12.0;
    else {
        tod->hours = fmod(tod->hours, 24.0);
        if (tod->hours < 0.0)
            tod->hours += 24.0;
    }
    if (!isfinite(tod->day_length_seconds) || tod->day_length_seconds < 0.0)
        tod->day_length_seconds = 0.0;
    if (!isfinite(tod->latitude_degrees) || tod->latitude_degrees < -85.0 ||
        tod->latitude_degrees > 85.0)
        tod->latitude_degrees = 35.0;
    if (!isfinite(tod->refresh_degrees) || tod->refresh_degrees <= 0.1 ||
        tod->refresh_degrees > 45.0)
        tod->refresh_degrees = 2.0;
    tod->has_refresh_dir = tod->has_refresh_dir ? 1 : 0;
    if (tod->has_refresh_dir) {
        double scale = fmax(fabs(tod->last_refresh_dir[0]),
                            fmax(fabs(tod->last_refresh_dir[1]), fabs(tod->last_refresh_dir[2])));
        if (!isfinite(scale) || scale <= 1e-12) {
            tod->has_refresh_dir = 0;
        } else {
            double x = tod->last_refresh_dir[0] / scale;
            double y = tod->last_refresh_dir[1] / scale;
            double z = tod->last_refresh_dir[2] / scale;
            double len = sqrt(x * x + y * y + z * z);
            if (!isfinite(len) || len <= 1e-12) {
                tod->has_refresh_dir = 0;
            } else {
                tod->last_refresh_dir[0] = x / len;
                tod->last_refresh_dir[1] = y / len;
                tod->last_refresh_dir[2] = z / len;
            }
        }
    }
    if (tod->sun_light && !rt_g3d_has_class(tod->sun_light, RT_G3D_LIGHT3D_CLASS_ID))
        rt_g3d_ref_slot_clear_unowned(&tod->sun_light);
    if (tod->sky && !rt_g3d_has_class(tod->sky, RT_G3D_SKY3D_CLASS_ID))
        rt_g3d_ref_slot_clear_unowned(&tod->sky);
    if (tod->probe && !rt_g3d_has_class(tod->probe, RT_G3D_REFLECTIONPROBE3D_CLASS_ID))
        rt_g3d_ref_slot_clear_unowned(&tod->probe);
}

/// @brief Release every light, sky, and reflection-probe reference owned by a clock.
/// @param obj TimeOfDay3D runtime object being finalized; NULL is accepted.
static void timeofday3d_finalize(void *obj) {
    rt_timeofday3d *tod = (rt_timeofday3d *)obj;
    if (!tod)
        return;
    timeofday3d_release(&tod->sun_light, RT_G3D_LIGHT3D_CLASS_ID);
    timeofday3d_release(&tod->sky, RT_G3D_SKY3D_CLASS_ID);
    timeofday3d_release(&tod->probe, RT_G3D_REFLECTIONPROBE3D_CLASS_ID);
}

/// @brief Allocate a paused noon clock with default latitude and refresh threshold.
/// @return New GC-managed TimeOfDay3D handle, or NULL after trapping on allocation failure.
void *rt_timeofday3d_new(void) {
    rt_timeofday3d *tod = (rt_timeofday3d *)rt_obj_new_i64(RT_G3D_TIMEOFDAY3D_CLASS_ID,
                                                           (int64_t)sizeof(rt_timeofday3d));
    if (!tod) {
        rt_trap("TimeOfDay3D.New: allocation failed");
        return NULL;
    }
    memset(tod, 0, sizeof(*tod));
    rt_obj_set_finalizer(tod, timeofday3d_finalize);
    tod->hours = 12.0;
    tod->day_length_seconds = 0.0;
    tod->latitude_degrees = 35.0;
    tod->refresh_degrees = 2.0;
    return tod;
}

/// @brief Validate a runtime handle as TimeOfDay3D and trap with calling-method context on failure.
/// @param obj Candidate runtime object handle.
/// @param method Trap message identifying the calling API.
/// @return Typed TimeOfDay3D pointer, or NULL when validation fails.
static rt_timeofday3d *timeofday3d_checked(void *obj, const char *method) {
    rt_timeofday3d *tod =
        (rt_timeofday3d *)rt_g3d_checked_or_null(obj, RT_G3D_TIMEOFDAY3D_CLASS_ID);
    if (!tod)
        rt_trap(method);
    return tod;
}

/// @brief Set the clock hour, wrapping finite input into the half-open day interval `[0, 24)`.
/// @param obj TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param hours New hour value; non-finite input is ignored.
void rt_timeofday3d_set_hours(void *obj, double hours) {
    rt_timeofday3d *tod = timeofday3d_checked(obj, "TimeOfDay3D.set_Hours: invalid clock");
    if (!tod || !isfinite(hours))
        return;
    hours = fmod(hours, 24.0);
    if (hours < 0.0)
        hours += 24.0;
    tod->hours = hours;
}

/// @brief Return the current wrapped hour of day.
/// @param obj TimeOfDay3D handle; invalid handles trap.
/// @return Hour in `[0, 24)`, or zero when validation fails.
double rt_timeofday3d_get_hours(void *obj) {
    rt_timeofday3d *tod = timeofday3d_checked(obj, "TimeOfDay3D.get_Hours: invalid clock");
    timeofday3d_repair_state(tod);
    return tod ? tod->hours : 0.0;
}

/// @brief Set the simulated number of real-time seconds in one full day.
/// @param obj TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param seconds Nonnegative day length; zero pauses automatic advancement.
void rt_timeofday3d_set_day_length_seconds(void *obj, double seconds) {
    rt_timeofday3d *tod =
        timeofday3d_checked(obj, "TimeOfDay3D.set_DayLengthSeconds: invalid clock");
    if (tod && isfinite(seconds) && seconds >= 0.0)
        tod->day_length_seconds = seconds;
}

/// @brief Return the configured duration of a simulated day.
/// @param obj TimeOfDay3D handle; invalid handles trap.
/// @return Nonnegative day duration in seconds, or zero when paused or invalid.
double rt_timeofday3d_get_day_length_seconds(void *obj) {
    rt_timeofday3d *tod =
        timeofday3d_checked(obj, "TimeOfDay3D.get_DayLengthSeconds: invalid clock");
    timeofday3d_repair_state(tod);
    return tod ? tod->day_length_seconds : 0.0;
}

/// @brief Set the latitude used by the stylized solar-elevation model.
/// @param obj TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param degrees Latitude in the accepted range `[-85, 85]`; other values are ignored.
void rt_timeofday3d_set_latitude_degrees(void *obj, double degrees) {
    rt_timeofday3d *tod =
        timeofday3d_checked(obj, "TimeOfDay3D.set_LatitudeDegrees: invalid clock");
    if (tod && isfinite(degrees) && degrees >= -85.0 && degrees <= 85.0)
        tod->latitude_degrees = degrees;
}

/// @brief Return the latitude used to derive solar elevation.
/// @param obj TimeOfDay3D handle; invalid handles trap.
/// @return Latitude in degrees, or zero when validation fails.
double rt_timeofday3d_get_latitude_degrees(void *obj) {
    rt_timeofday3d *tod =
        timeofday3d_checked(obj, "TimeOfDay3D.get_LatitudeDegrees: invalid clock");
    timeofday3d_repair_state(tod);
    return tod ? tod->latitude_degrees : 0.0;
}

/// @brief Set the angular sun movement required to refresh sky and reflection consumers.
/// @param obj TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param degrees Threshold greater than 0.1 degrees, clamped to 45; smaller/invalid values are
/// ignored.
void rt_timeofday3d_set_refresh_degrees(void *obj, double degrees) {
    rt_timeofday3d *tod = timeofday3d_checked(obj, "TimeOfDay3D.set_RefreshDegrees: invalid clock");
    if (tod && isfinite(degrees) && degrees > 0.1)
        tod->refresh_degrees = degrees > 45.0 ? 45.0 : degrees;
}

/// @brief Return the angular refresh threshold.
/// @param obj TimeOfDay3D handle; invalid handles trap.
/// @return Threshold in degrees, or zero when validation fails.
double rt_timeofday3d_get_refresh_degrees(void *obj) {
    rt_timeofday3d *tod = timeofday3d_checked(obj, "TimeOfDay3D.get_RefreshDegrees: invalid clock");
    timeofday3d_repair_state(tod);
    return tod ? tod->refresh_degrees : 0.0;
}

/// @brief Validate, retain, and assign one bound runtime consumer.
/// @param slot Address of the retained consumer slot.
/// @param value New consumer handle, or NULL to clear the binding.
/// @param class_id Required runtime class identifier.
/// @param trap Diagnostic emitted when @p value has the wrong class.
static void timeofday3d_bind(void **slot, void *value, int64_t class_id, const char *trap) {
    if (value && !rt_g3d_checked_or_null(value, class_id)) {
        rt_trap(trap);
        return;
    }
    if (*slot == value)
        return;
    if (value)
        rt_obj_retain_maybe(value);
    timeofday3d_release(slot, class_id);
    *slot = value;
}

/// @brief Bind the directional Light3D driven by the clock's sun model.
/// @param obj TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param light Light3D handle to retain, or NULL to clear the binding.
void rt_timeofday3d_set_sun_light(void *obj, void *light) {
    rt_timeofday3d *tod = timeofday3d_checked(obj, "TimeOfDay3D.SetSunLight: invalid clock");
    if (tod)
        timeofday3d_bind(&tod->sun_light,
                         light,
                         RT_G3D_LIGHT3D_CLASS_ID,
                         "TimeOfDay3D.SetSunLight: light must be a Light3D");
}

/// @brief Bind the procedural Sky3D refreshed as the sun moves.
/// @param obj TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param sky Sky3D handle to retain, or NULL to clear the binding.
void rt_timeofday3d_set_sky(void *obj, void *sky) {
    rt_timeofday3d *tod = timeofday3d_checked(obj, "TimeOfDay3D.SetSky: invalid clock");
    if (tod)
        timeofday3d_bind(
            &tod->sky, sky, RT_G3D_SKY3D_CLASS_ID, "TimeOfDay3D.SetSky: sky must be a Sky3D");
}

/// @brief Bind the ReflectionProbe3D marked dirty as the sun moves.
/// @param obj TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param probe ReflectionProbe3D handle to retain, or NULL to clear the binding.
void rt_timeofday3d_set_reflection_probe(void *obj, void *probe) {
    rt_timeofday3d *tod = timeofday3d_checked(obj, "TimeOfDay3D.SetReflectionProbe: invalid clock");
    if (tod)
        timeofday3d_bind(&tod->probe,
                         probe,
                         RT_G3D_REFLECTIONPROBE3D_CLASS_ID,
                         "TimeOfDay3D.SetReflectionProbe: probe must be a ReflectionProbe3D");
}

/// @brief Compute the direction TOWARD the sun for the current hour/latitude.
/// @param obj TimeOfDay3D handle; invalid handles trap.
/// @param out_dir Three-element output array receiving a normalized direction.
void rt_timeofday3d_get_sun_direction_raw(void *obj, double out_dir[3]) {
    if (out_dir) {
        out_dir[0] = 0.0;
        out_dir[1] = 1.0;
        out_dir[2] = 0.0;
    }
    rt_timeofday3d *tod = timeofday3d_checked(obj, "TimeOfDay3D.SunDirection: invalid clock");
    if (!tod || !out_dir)
        return;
    timeofday3d_repair_state(tod);
    /* Hour angle: 12h = solar noon; the sun wheels east (-X at 6h) to west.
     * The horizontal component traces a full CIRCLE over the day (bearing =
     * hour angle), so both the direction and its time-derivative stay
     * continuous across the 24h→0h wrap — at midnight the sun/moon vector
     * points north behind the world instead of snapping back through the
     * south meridian. Matches the old stylized daytime arc at noon exactly
     * and stays within a few degrees of it through the morning/afternoon. */
    double hour_angle = (tod->hours - 12.0) / 12.0 * 3.14159265358979323846;
    double lat = tod->latitude_degrees * (3.14159265358979323846 / 180.0);
    double elevation = cos(hour_angle) * cos(lat) * 0.9 + 0.1 * sin(lat);
    double horizontal = sqrt(fmax(0.0, 1.0 - elevation * elevation));
    out_dir[0] = -horizontal * sin(hour_angle);
    out_dir[1] = elevation;
    out_dir[2] = -horizontal * cos(hour_angle) * 0.4 - 0.1;
    double len = sqrt(out_dir[0] * out_dir[0] + out_dir[1] * out_dir[1] + out_dir[2] * out_dir[2]);
    if (!isfinite(len) || len <= 1e-12) {
        out_dir[0] = 0.0;
        out_dir[1] = 1.0;
        out_dir[2] = 0.0;
        return;
    }
    out_dir[0] /= len;
    out_dir[1] /= len;
    out_dir[2] /= len;
}

/// @brief Return the current normalized direction toward the sun as a new Vec3.
/// @param obj TimeOfDay3D handle; invalid handles trap.
/// @return New GC-managed Vec3 direction, or NULL when Vec3 allocation fails.
void *rt_timeofday3d_get_sun_direction(void *obj) {
    double dir[3] = {0.0, 1.0, 0.0};
    rt_timeofday3d_get_sun_direction_raw(obj, dir);
    return rt_vec3_new(dir[0], dir[1], dir[2]);
}

/// @brief Advance the clock by @p dt seconds and drive every bound consumer.
/// @details Call once per frame with the world's (scaled) dt — deterministic and
///   replay-stable. With dayLengthSeconds 0 the clock is paused but bound
///   consumers still refresh when `hours` was set manually. The sun light gets
///   direction plus an elevation-keyed warm-low/white-noon color curve (off below
///   the horizon); the sky and probe refresh only when the sun has moved more
///   than RefreshDegrees since the last refresh (IBL/capture cost throttle).
/// @param obj TimeOfDay3D handle; invalid handles trap and are ignored.
/// @param dt Positive finite scaled simulation delta in seconds; invalid values do not advance
/// time.
/// @param canvas Optional Canvas3D handle on which a refreshed procedural sky is installed.
void rt_timeofday3d_advance(void *obj, double dt, void *canvas) {
    rt_timeofday3d *tod = timeofday3d_checked(obj, "TimeOfDay3D.Advance: invalid clock");
    if (!tod)
        return;
    timeofday3d_repair_state(tod);
    if (isfinite(dt) && dt > 0.0 && tod->day_length_seconds > 0.0) {
        double remainder = fmod(dt, tod->day_length_seconds);
        double advance_hours = remainder / tod->day_length_seconds * 24.0;
        if (!isfinite(advance_hours))
            advance_hours = 0.0;
        tod->hours += advance_hours;
        tod->hours = fmod(tod->hours, 24.0);
        if (tod->hours < 0.0)
            tod->hours += 24.0;
    }
    double dir[3];
    rt_timeofday3d_get_sun_direction_raw(tod, dir);

    if (tod->sun_light) {
        void *light_dir = rt_vec3_new(-dir[0], -dir[1], -dir[2]); /* travel direction */
        if (light_dir) {
            rt_light3d_set_direction(tod->sun_light, light_dir);
            if (rt_obj_release_check0(light_dir))
                rt_obj_free(light_dir);
        }
        double elev = dir[1];
        if (elev <= 0.0) {
            rt_light3d_set_intensity(tod->sun_light, 0.0);
        } else {
            /* Warm at the horizon, white at noon. */
            double warm = elev < 0.35 ? 1.0 - elev / 0.35 : 0.0;
            rt_light3d_set_color(tod->sun_light, 1.0, 0.95 - 0.35 * warm, 0.90 - 0.60 * warm);
            double intensity = elev < 0.2 ? elev / 0.2 : 1.0;
            rt_light3d_set_intensity(tod->sun_light, intensity);
        }
    }

    double moved_cos = tod->has_refresh_dir
                           ? dir[0] * tod->last_refresh_dir[0] + dir[1] * tod->last_refresh_dir[1] +
                                 dir[2] * tod->last_refresh_dir[2]
                           : -1.0;
    double threshold_cos = cos(tod->refresh_degrees * (3.14159265358979323846 / 180.0));
    if (!tod->has_refresh_dir || moved_cos < threshold_cos) {
        int refresh_ok = 1;
        if (tod->sky) {
            void *sun_vec = rt_vec3_new(dir[0], dir[1], dir[2]);
            if (sun_vec) {
                rt_sky3d_set_sun_direction(tod->sky, sun_vec);
                if (rt_obj_release_check0(sun_vec))
                    rt_obj_free(sun_vec);
                if (!rt_sky3d_update(tod->sky, canvas))
                    refresh_ok = 0;
            } else {
                refresh_ok = 0;
            }
        }
        if (tod->probe)
            rt_reflectionprobe3d_set_capture_dirty(tod->probe, 1);
        if (refresh_ok) {
            memcpy(tod->last_refresh_dir, dir, sizeof(tod->last_refresh_dir));
            tod->has_refresh_dir = 1;
        }
    }
}

#else
typedef int rt_timeofday3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
