//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_tween.h
/// @file
/// @brief Declares the frame-counted Tween object and its scalar easing
///        utilities.
// Purpose: Frame-based tweening system with configurable easing functions for smooth animations,
// supporting double and int64 value types, play/pause/stop lifecycle, and static lerp helpers.
//
// Key invariants:
//   - Start operations clamp durations below one to one frame and replace
//     invalid easing identifiers with RT_EASE_LINEAR.
//   - Progress is in [0, 100]; value equals 'from' at start and 'to' at completion.
//   - rt_tween_update must be called once per frame.
//   - StartI64 retains exact integer endpoints for ValueI64; intermediate
//     frames are eased and rounded half away from zero with saturation.
//   - Static lerp and easing functions are pure and allocation-free.
//
// Ownership/Lifetime:
//   - Tween handles are reference-counted GC objects. rt_tween_destroy releases
//     the caller's reference and frees the object when that was the last one.
//   - Unless explicitly identified as owned, handle parameters are borrowed and
//     are valid only for the duration of the call.
//   - Static functions require no allocation and have no ownership semantics.
//
// Links: src/runtime/game/rt_tween.c (implementation), src/runtime/core/rt_easing.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime object class identifier used to validate Tween handles.
#define RT_TWEEN_CLASS_ID INT64_C(-0x510202)

/// @brief Easing curves supported by rt_tween_start() and rt_tween_ease().
/// @details The enumerators form a contiguous dispatch range beginning at zero.
///          RT_EASE_COUNT is a sentinel and is not itself a curve.
typedef enum {
    RT_EASE_LINEAR = 0,    ///< Linear interpolation (no easing)
    RT_EASE_IN_QUAD,       ///< Quadratic ease-in
    RT_EASE_OUT_QUAD,      ///< Quadratic ease-out
    RT_EASE_IN_OUT_QUAD,   ///< Quadratic ease-in-out
    RT_EASE_IN_CUBIC,      ///< Cubic ease-in
    RT_EASE_OUT_CUBIC,     ///< Cubic ease-out
    RT_EASE_IN_OUT_CUBIC,  ///< Cubic ease-in-out
    RT_EASE_IN_SINE,       ///< Sinusoidal ease-in
    RT_EASE_OUT_SINE,      ///< Sinusoidal ease-out
    RT_EASE_IN_OUT_SINE,   ///< Sinusoidal ease-in-out
    RT_EASE_IN_EXPO,       ///< Exponential ease-in
    RT_EASE_OUT_EXPO,      ///< Exponential ease-out
    RT_EASE_IN_OUT_EXPO,   ///< Exponential ease-in-out
    RT_EASE_IN_BACK,       ///< Back ease-in (overshoots)
    RT_EASE_OUT_BACK,      ///< Back ease-out (overshoots)
    RT_EASE_IN_OUT_BACK,   ///< Back ease-in-out
    RT_EASE_IN_BOUNCE,     ///< Bounce ease-in
    RT_EASE_OUT_BOUNCE,    ///< Bounce ease-out
    RT_EASE_IN_OUT_BOUNCE, ///< Bounce ease-in-out
    RT_EASE_COUNT          ///< Number of easing types
} rt_ease_type_t;

/// @brief Opaque handle to a reference-counted Tween runtime object.
typedef struct rt_tween_impl *rt_tween;

/// @brief Allocates and initializes a new Tween in the idle state.
/// @details The new object has zero values and duration, uses linear easing,
///          and does not advance until a start operation configures it.
/// @return Owned Tween handle, or `NULL` if allocation fails. Release the
///         returned reference with rt_tween_destroy().
rt_tween rt_tween_new(void);

/// @brief Releases one owned reference to a Tween.
/// @details The storage is reclaimed when this is the final reference.
/// @param tween Owned Tween handle to release; `NULL` is ignored.
void rt_tween_destroy(rt_tween tween);

/// @brief Begins a tween animation interpolating between two double values.
/// @details Replaces all playback state and begins at @p from with zero elapsed
///          frames. A non-finite @p from becomes zero; a non-finite @p to
///          becomes the sanitized start value. Durations below one become one,
///          and out-of-range easing identifiers select RT_EASE_LINEAR.
/// @param tween Borrowed Tween handle to configure.
/// @param from Starting value of the interpolation.
/// @param to Ending value of the interpolation.
/// @param duration Total duration in calls to rt_tween_update().
/// @param ease_type Easing curve identifier in the `RT_EASE_*` range.
void rt_tween_start(rt_tween tween, double from, double to, int64_t duration, int64_t ease_type);

/// @brief Begins a tween animation interpolating between two integer values.
/// @details Uses the same playback machinery and input normalization as
///          rt_tween_start(), while retaining exact int64 endpoints so
///          rt_tween_value_i64() does not lose them through a double round-trip.
/// @param tween Borrowed Tween handle to configure.
/// @param from Exact starting integer value.
/// @param to Exact ending integer value.
/// @param duration Total duration in calls to rt_tween_update(); values below
///        one are clamped.
/// @param ease_type Easing curve identifier; invalid values select linear.
void rt_tween_start_i64(
    rt_tween tween, int64_t from, int64_t to, int64_t duration, int64_t ease_type);

/// @brief Advances the tween by one game frame.
/// @details Running, unpaused tweens increment elapsed time, apply their easing
///          curve, and update the current value. The completion update pins the
///          value to the exact double endpoint and stops playback. Other states
///          are left unchanged.
/// @param tween Borrowed Tween handle to advance.
/// @return `1` only when this call reaches the configured duration; `0` for
///         every other state, including null, paused, and completed tweens.
int8_t rt_tween_update(rt_tween tween);

/// @brief Retrieves the current interpolated value as a double.
/// @param tween Borrowed Tween handle to query.
/// @return Current eased value, the start value before the first update, the
///         endpoint after completion, or `0.0` for `NULL`.
double rt_tween_value(rt_tween tween);

/// @brief Retrieves the current interpolated value as a rounded integer.
/// @details Integer-started tweens interpolate from their retained exact
///          endpoints and preserve those endpoints exactly. Double-started
///          tweens round the current double half away from zero. Both paths
///          saturate results that exceed the int64 range.
/// @param tween Borrowed Tween handle to query.
/// @return Current integer value, or `0` for `NULL`.
int64_t rt_tween_value_i64(rt_tween tween);

/// @brief Queries whether the tween is currently running (not paused, not
///   completed).
/// @param tween Borrowed Tween handle to query.
/// @return `1` if actively interpolating; `0` if null, idle, stopped, paused,
///         or completed.
int8_t rt_tween_is_running(rt_tween tween);

/// @brief Queries whether the tween has reached its end value.
/// @details Only natural completion sets this latch. Stopping preserves its
///          existing value; starting or resetting clears it.
/// @param tween Borrowed Tween handle to query.
/// @return `1` when the completion latch is set; otherwise `0`.
int8_t rt_tween_is_complete(rt_tween tween);

/// @brief Retrieves the tween progress as an integer percentage.
/// @details Progress is based on elapsed frames rather than eased position and
///          uses truncating integer conversion.
/// @param tween Borrowed Tween handle to query.
/// @return Value in the inclusive range 0..100, or `0` for null/unstarted.
int64_t rt_tween_progress(rt_tween tween);

/// @brief Retrieves the number of frames elapsed since the tween started.
/// @param tween Borrowed Tween handle to query.
/// @return Elapsed update frames, or `0` for `NULL`.
int64_t rt_tween_elapsed(rt_tween tween);

/// @brief Retrieves the total duration of the tween.
/// @param tween Borrowed Tween handle to query.
/// @return Configured update-frame duration, or `0` for null/unstarted.
int64_t rt_tween_duration(rt_tween tween);

/// @brief Stops the tween at its current value and clears pause state.
/// @details Preserves current value, elapsed time, endpoints, and the existing
///          completion latch.
/// @param tween Borrowed Tween handle to stop.
void rt_tween_stop(rt_tween tween);

/// @brief Resets the tween to the beginning without changing its from/to
///   or duration settings.
/// @details Clears elapsed time, completion, and pause state. A configured
///          tween with positive duration starts running again; an unstarted
///          object remains inactive.
/// @param tween Borrowed Tween handle to reset.
void rt_tween_reset(rt_tween tween);

/// @brief Pauses the tween at its current position.
/// @details Running, incomplete tweens retain their value and elapsed time.
///          Inactive and completed tweens are unchanged.
/// @param tween Borrowed Tween handle to pause.
void rt_tween_pause(rt_tween tween);

/// @brief Resumes a paused tween from its current position.
/// @details Clears the pause flag but does not restart a stopped or completed
///          tween.
/// @param tween Borrowed Tween handle to resume.
void rt_tween_resume(rt_tween tween);

/// @brief Queries whether the tween is currently paused.
/// @param tween Borrowed Tween handle to query.
/// @return `1` when the pause flag is set; otherwise `0`.
int8_t rt_tween_is_paused(rt_tween tween);

//=============================================================================
// Static interpolation functions (no Tween instance needed)
//=============================================================================

/// @brief Performs integer linear interpolation between two values.
/// @details The computation retains exact endpoint values, uses long-double
///          intermediate arithmetic, rounds halves away from zero, and
///          saturates results outside the int64 range.
/// @param from Exact starting value.
/// @param to Exact ending value.
/// @param t Interpolation progress from 0.0 (returns @p from) to 1.0
///        (returns @p to). Non-finite and nonpositive values select @p from;
///        values at least one select @p to.
/// @return Rounded, saturated interpolated value.
int64_t rt_tween_lerp_i64(int64_t from, int64_t to, double t);

/// @brief Applies an easing function to a normalized progress value.
/// @details Non-finite and nonpositive progress maps to zero, and progress at
///          least one maps to one, guaranteeing exact endpoints. Unknown curve
///          identifiers fall back to linear progress.
/// @param t Linear progress, normally in the inclusive range 0.0..1.0.
/// @param ease_type Easing curve identifier in the `RT_EASE_*` range.
/// @return Eased progress. Back curves may temporarily fall below zero or
///         exceed one.
double rt_tween_ease(double t, int64_t ease_type);

#ifdef __cplusplus
}
#endif
