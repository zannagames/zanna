//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_tween.c
/// @file
/// @brief Implements frame-counted scalar tweening, exact integer endpoints,
///        lifecycle controls, and nineteen easing curves.
// Purpose: Frame-counted value interpolation ("tweening") for Zanna games and
//   UIs. A Tween smoothly animates a scalar value from a start to an end over
//   a specified number of frames, optionally applying one of 19 easing curves
//   (linear, quad, cubic, sine, exponential, back-overshoot, and bounced
//   variants). Typical uses: moving UI panels, fading colors, scaling entities,
//   and any animation that must complete in a predictable number of frames.
//
// Key invariants:
//   - Time is measured in integer frames (not wall-clock milliseconds).
//     Duration must be >= 1; zero or negative durations are clamped to 1.
//   - The tween progresses by calling rt_tween_update() once per frame.
//     Update returns 1 on the frame the tween completes, 0 otherwise.
//     After completion, is_complete() returns 1 and the value is pinned to `to`.
//   - Easing functions operate on a normalized progress t ∈ [0.0, 1.0]:
//       Linear:  f(t) = t
//       In-Quad: f(t) = t²     (starts slow, ends fast)
//       Back:    overshoots the target slightly before settling (c1 = 1.70158)
//       Bounce:  simulates a physical bounce using piecewise polynomials
//     The `ease_type` parameter is one of the RT_EASE_* constants defined in
//     rt_tween.h. Unknown types fall back to linear.
//   - Pause/Resume halt and resume update progression without resetting elapsed.
//   - rt_tween_reset() rewinds to frame 0 and resumes from the original `from`.
//   - rt_tween_value_i64() rounds halves away from zero and saturates at the
//     int64 limits. For StartI64 tweens it is anchored on the exact int64
//     endpoints, so the start value, the end value, and constant (from == to)
//     tweens are preserved without the 2^53 double-precision loss; eased
//     intermediate frames still interpolate in long double.
//
// Ownership/Lifetime:
//   - Tween objects are GC-managed (rt_obj_new_i64). rt_tween_destroy() calls
//     rt_obj_free() for callers that manage lifetimes explicitly; the GC also
//     collects them automatically.
//
// Links: src/runtime/game/rt_tween.h (public API, easing constants),
//        docs/zannalib/game.md (Tween and TweenChain sections)
//
//===----------------------------------------------------------------------===//

#include "rt_tween.h"
#include "rt_object.h"
#include "rt_trap.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/// @brief Mutable interpolation state owned by a Tween runtime object.
struct rt_tween_impl {
    double from;       ///< Starting value.
    double to;         ///< Ending value.
    double current;    ///< Current interpolated value.
    int64_t from_i64;  ///< Exact integer start (valid when is_i64); anchors ValueI64.
    int64_t to_i64;    ///< Exact integer end (valid when is_i64); anchors ValueI64.
    int64_t duration;  ///< Total duration in frames.
    int64_t elapsed;   ///< Elapsed frames.
    int64_t ease_type; ///< Easing function type.
    int8_t running;    ///< 1 if tween is running.
    int8_t complete;   ///< 1 if tween has completed.
    int8_t paused;     ///< 1 if tween is paused.
    int8_t is_i64;     ///< 1 if started via StartI64 (ValueI64 reads exact endpoints).
    int8_t ms_mode;    ///< 1 when duration/elapsed are milliseconds, not frames.
    int8_t reduce_motion; ///< 1 to snap to the target on Start (accessibility).
};

/// @brief Safe-cast a handle to the Tween impl, trapping @p api on a class-id
///        mismatch.
/// @param tween Borrowed candidate Tween handle.
/// @param api Trap message identifying the calling API.
/// @return Borrowed implementation pointer, or `NULL` when @p tween is `NULL`.
static rt_tween checked_tween(rt_tween tween, const char *api) {
    if (!tween)
        return NULL;
    if (rt_obj_class_id(tween) != RT_TWEEN_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return tween;
}

/// @brief Return @p value if finite, else @p fallback (NaN/Inf sanitizer).
/// @param value Candidate floating-point value.
/// @param fallback Replacement for NaN or infinity.
/// @return @p value when finite; otherwise @p fallback.
static double tween_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Linear interpolation that avoids overflowing the endpoint delta for
///        large opposite-signed values.
/// @param from Start endpoint.
/// @param to End endpoint.
/// @param t Interpolation parameter; non-finite input becomes zero.
/// @return Finite weighted value when possible, otherwise the nearer endpoint.
static double tween_lerp_double(double from, double to, double t) {
    if (!isfinite(t))
        t = 0.0;
    if (t == 0.0)
        return from;
    if (t == 1.0)
        return to;

    double result = from * (1.0 - t) + to * t;
    if (isfinite(result))
        return result;

    result = from + (to - from) * t;
    if (isfinite(result))
        return result;

    return t < 0.5 ? from : to;
}

/// @brief Round-half-away-from-zero to int64, saturating; 0 for non-finite.
/// @param value Floating-point value to convert.
/// @return Nearest integer with ties away from zero, saturated to int64.
static int64_t tween_round_to_i64(double value) {
    if (!isfinite(value))
        return 0;
    if (value >= (double)INT64_MAX)
        return INT64_MAX;
    if (value <= (double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)(value + (value >= 0 ? 0.5 : -0.5));
}

/// @brief Interpolate between exact integer endpoints at fraction @p frac, rounded
///        half-away-from-zero and saturated. Anchored on the int64 endpoints so a
///        fraction of 0 returns @p from exactly, 1 returns @p to exactly, and a
///        constant (from == to) never drifts — unlike casting the endpoints to
///        double first, which loses bits above 2^53 (VDOC-273). Intermediate
///        fractions use long double to minimize precision loss on wide ranges.
/// @param from Exact integer start endpoint.
/// @param to Exact integer end endpoint.
/// @param frac Interpolation fraction, normally produced by an easing curve.
/// @return Rounded, saturated interpolated value with exact endpoint anchors.
static int64_t tween_lerp_endpoints_i64(int64_t from, int64_t to, double frac) {
    if (from == to || frac == 0.0)
        return from;
    if (frac == 1.0)
        return to;
    long double delta = (long double)to - (long double)from;
    long double result = (long double)from + (long double)frac * delta;
    if (!isfinite((double)result))
        return frac < 0.5 ? from : to;
    long double rounded = result >= 0.0L ? floorl(result + 0.5L) : ceill(result - 0.5L);
    if (rounded >= (long double)INT64_MAX)
        return INT64_MAX;
    if (rounded <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)rounded;
}

/// @brief Integer percentage value*100/total, clamped to [0, 100]; 0 for
///        non-positive inputs.
/// @param value Non-negative elapsed frame count.
/// @param total Positive duration.
/// @return Truncated percentage in the inclusive range 0..100.
static int64_t tween_percent_i64(int64_t value, int64_t total) {
    if (value <= 0 || total <= 0)
        return 0;
    long double scaled = ((long double)value * 100.0L) / (long double)total;
    int64_t pct = scaled >= (long double)INT64_MAX ? INT64_MAX : (int64_t)scaled;
    return pct > 100 ? 100 : pct;
}

// Forward declaration of public easing function
/// @brief Apply an easing curve to a linear progress value t in [0,1].
/// @param t Linear progress.
/// @param ease_type One of the `RT_EASE_*` identifiers.
/// @return Eased progress with exact clamped endpoints.
double rt_tween_ease(double t, int64_t ease_type);

// Forward declaration of internal easing functions
// NOTE: These duplicate rt_easing.c implementations. A future refactor could
// have rt_tween call the public rt_ease_* API instead.
static double ease_linear(double t);
static double ease_in_quad(double t);
static double ease_out_quad(double t);
static double ease_in_out_quad(double t);
static double ease_in_cubic(double t);
static double ease_out_cubic(double t);
static double ease_in_out_cubic(double t);
static double ease_in_sine(double t);
static double ease_out_sine(double t);
static double ease_in_out_sine(double t);
static double ease_in_expo(double t);
static double ease_out_expo(double t);
static double ease_in_out_expo(double t);
static double ease_in_back(double t);
static double ease_out_back(double t);
static double ease_in_out_back(double t);
static double ease_in_bounce(double t);
static double ease_out_bounce(double t);
static double ease_in_out_bounce(double t);

/// @brief Create a new tween interpolator (starts inactive until start() is called).
/// @return Owned Tween handle, or `NULL` if allocation fails.
rt_tween rt_tween_new(void) {
    struct rt_tween_impl *tween = (struct rt_tween_impl *)rt_obj_new_i64(
        RT_TWEEN_CLASS_ID, (int64_t)sizeof(struct rt_tween_impl));
    if (!tween)
        return NULL;

    tween->from = 0.0;
    tween->to = 0.0;
    tween->current = 0.0;
    tween->from_i64 = 0;
    tween->to_i64 = 0;
    tween->duration = 0;
    tween->elapsed = 0;
    tween->ease_type = RT_EASE_LINEAR;
    tween->running = 0;
    tween->complete = 0;
    tween->ms_mode = 0;
    tween->reduce_motion = 0;
    tween->paused = 0;
    tween->is_i64 = 0;

    return tween;
}

/// @brief Release one owned Tween reference.
/// @param tween Owned handle to release; `NULL` is ignored.
void rt_tween_destroy(rt_tween tween) {
    tween = checked_tween(tween, "Tween.Destroy: expected Zanna.Game.Tween");
    if (tween && rt_obj_release_check0(tween))
        rt_obj_free(tween);
}

/// @brief Start interpolating from one value to another over the given duration with easing.
/// @details Non-finite start becomes zero, non-finite end becomes the start,
///          durations below one clamp to one, and unknown easing IDs become
///          linear. Existing playback state is replaced.
/// @param tween Borrowed Tween handle.
/// @param from Start value.
/// @param to End value.
/// @param duration Duration in update frames.
/// @param ease_type One of the `RT_EASE_*` identifiers.
void rt_tween_start(rt_tween tween, double from, double to, int64_t duration, int64_t ease_type) {
    tween = checked_tween(tween, "Tween.Start: expected Zanna.Game.Tween");
    if (!tween)
        return;
    from = tween_finite_or(from, 0.0);
    to = tween_finite_or(to, from);
    if (duration < 1)
        duration = 1;
    if (ease_type < 0 || ease_type >= RT_EASE_COUNT)
        ease_type = RT_EASE_LINEAR;

    tween->from = from;
    tween->to = to;
    tween->current = from;
    tween->duration = duration;
    tween->elapsed = 0;
    tween->ease_type = ease_type;
    tween->running = 1;
    tween->complete = 0;
    tween->paused = 0;
    tween->is_i64 = 0; // double-domain tween; ValueI64 falls back to rounding current
    tween->ms_mode = 0;
    // Accessibility: a reduce-motion tween lands on its target immediately and
    // reports complete, so callers need no special-casing at the call site.
    if (tween->reduce_motion) {
        tween->elapsed = duration;
        tween->current = to;
        tween->running = 0;
        tween->complete = 1;
    }
}

/// @brief Start an integer-valued tween. The double machinery still drives eased
/// intermediate frames, but the exact int64 endpoints are retained so ValueI64
/// preserves them (and a constant tween) without the 2^53 precision loss that a
/// bare double cast would introduce (VDOC-273).
/// @param tween Borrowed Tween handle.
/// @param from Exact integer start endpoint.
/// @param to Exact integer end endpoint.
/// @param duration Duration in update frames, clamped to at least one.
/// @param ease_type Easing identifier, defaulting to linear when invalid.
void rt_tween_start_i64(
    rt_tween tween, int64_t from, int64_t to, int64_t duration, int64_t ease_type) {
    rt_tween_start(tween, (double)from, (double)to, duration, ease_type);
    // rt_tween_start clears is_i64; re-flag and record the exact endpoints after it.
    tween = checked_tween(tween, "Tween.StartI64: expected Zanna.Game.Tween");
    if (!tween)
        return;
    tween->from_i64 = from;
    tween->to_i64 = to;
    tween->is_i64 = 1;
}

/// @brief Advance the tween by one tick. Returns 1 if the tween just completed.
/// @details Paused, stopped, and completed tweens are unchanged. Active
///          playback advances one saturating frame, applies normalized easing,
///          interpolates, and pins the exact double end value on completion.
/// @param tween Borrowed Tween handle.
/// @return `1` only on the update that reaches the duration; otherwise `0`.
int8_t rt_tween_update(rt_tween tween) {
    tween = checked_tween(tween, "Tween.Update: expected Zanna.Game.Tween");
    if (!tween)
        return 0;
    // Enforce the tween's mode: the frame-based Update() is a no-op on a tween
    // started in millisecond mode, so a mismatched call cannot silently advance
    // a millisecond tween by one "frame" and reinterpret its units. Mirrors the
    // guard rt_timer_update already carries (VDOC-264).
    if (tween->ms_mode)
        return 0;
    if (!tween->running || tween->paused)
        return 0;

    if (tween->elapsed < INT64_MAX)
        tween->elapsed++;

    // Calculate progress (0.0 to 1.0)
    double t = (double)tween->elapsed / (double)tween->duration;
    if (t > 1.0)
        t = 1.0;

    // Apply easing
    double eased_t = rt_tween_ease(t, tween->ease_type);

    // Interpolate
    tween->current = tween_lerp_double(tween->from, tween->to, eased_t);

    // Check for completion
    if (tween->elapsed >= tween->duration) {
        tween->running = 0;
        tween->complete = 1;
        tween->current = tween->to; // Ensure exact end value
        return 1;                   // Just completed
    }

    return 0;
}


//=============================================================================
// Millisecond Mode (ADR 0250)
//=============================================================================
//
// `Update()` advances one *frame*, so a tween's duration is frame-count and its
// motion is frame-rate dependent. That is fine for a fixed-step game loop and
// useless for anything that must be reproducible under an arbitrary dt — a
// replay, a headless capture, a paced cutscene. `Zanna.Game.Timer` already
// carries both modes (`Start`/`Update` in frames, `StartMs`/`UpdateMs` in
// milliseconds); this brings `Tween` to the same shape.
//
// The mode is sticky: `UpdateMs` is a no-op on a frame-mode tween and `Update`
// is a no-op on a ms-mode tween, so a mismatched call cannot silently
// reinterpret the units.

/// @brief Recompute @p tween's current value from its elapsed/duration pair.
/// @details Shared by the millisecond advance and seek paths so both agree
///          exactly with each other. Completion pins the exact end value, which
///          matters because easing curves need not land on 1.0 numerically.
/// @param tween Validated, running tween.
/// @return `1` when this call reached the duration; otherwise `0`.
static int8_t tween_apply_elapsed(rt_tween tween) {
    double t = (double)tween->elapsed / (double)tween->duration;
    if (t > 1.0)
        t = 1.0;
    tween->current = tween_lerp_double(tween->from, tween->to, rt_tween_ease(t, tween->ease_type));
    if (tween->elapsed >= tween->duration) {
        tween->running = 0;
        tween->complete = 1;
        tween->current = tween->to;
        return 1;
    }
    return 0;
}

/// @brief Start a millisecond-timed tween.
/// @details Identical to @ref rt_tween_start except the duration is wall time,
///          advanced by @ref rt_tween_update_ms or positioned by
///          @ref rt_tween_seek_ms.
/// @param tween Borrowed Tween handle.
/// @param from Start value; non-finite becomes zero.
/// @param to End value; non-finite becomes @p from.
/// @param duration_ms Duration in milliseconds; values below one are clamped up.
/// @param ease_type Easing identifier, defaulting to linear when invalid.
void rt_tween_start_ms(
    rt_tween tween, double from, double to, int64_t duration_ms, int64_t ease_type) {
    rt_tween_start(tween, from, to, duration_ms, ease_type);
    tween = checked_tween(tween, "Tween.StartMs: expected Zanna.Game.Tween");
    if (!tween)
        return;
    tween->ms_mode = 1;
}

/// @brief Advance a millisecond-mode tween by @p dt_ms.
/// @details A non-positive @p dt_ms does not advance. Elapsed time saturates
///          rather than overflowing. No-op on a frame-mode tween.
/// @param tween Borrowed Tween handle.
/// @param dt_ms Elapsed milliseconds since the previous call.
/// @return `1` only on the call that reaches the duration; otherwise `0`.
int8_t rt_tween_update_ms(rt_tween tween, int64_t dt_ms) {
    tween = checked_tween(tween, "Tween.UpdateMs: expected Zanna.Game.Tween");
    if (!tween || !tween->ms_mode)
        return 0;
    if (!tween->running || tween->paused || dt_ms <= 0)
        return 0;
    if (tween->elapsed > INT64_MAX - dt_ms)
        tween->elapsed = INT64_MAX;
    else
        tween->elapsed += dt_ms;
    return tween_apply_elapsed(tween);
}

/// @brief Position a millisecond-mode tween at an absolute time.
/// @details **Stateless with respect to the path taken**: seeking to the same
///          millisecond always yields the same value, whatever sequence of
///          advances or seeks preceded it. That is the property a scrubbable
///          timeline or a fixed-step probe needs, and the reason a pure
///          `UpdateMs` accumulator is not sufficient on its own. Seeking
///          backwards un-completes the tween and resumes it. No-op on a
///          frame-mode tween.
/// @param tween Borrowed Tween handle.
/// @param ms Absolute position in milliseconds from the start; negative clamps
///        to zero.
/// @return `1` when the seek lands at or past the duration; otherwise `0`.
int8_t rt_tween_seek_ms(rt_tween tween, int64_t ms) {
    tween = checked_tween(tween, "Tween.SeekMs: expected Zanna.Game.Tween");
    if (!tween || !tween->ms_mode || tween->duration <= 0)
        return 0;
    if (ms < 0)
        ms = 0;
    tween->elapsed = ms;
    tween->complete = 0;
    tween->running = 1;
    return tween_apply_elapsed(tween);
}

/// @brief Eased progress in permille (0..1000).
/// @details Integer-exact, so it can drive a deterministic simulation without
///          the backend-dependent rounding an `f64` progress would introduce.
/// @param tween Borrowed Tween handle.
/// @return Eased progress scaled to `[0, 1000]`, or zero for null.
int64_t rt_tween_progress_permille(rt_tween tween) {
    tween = checked_tween(tween, "Tween.ProgressPermille: expected Zanna.Game.Tween");
    if (!tween || tween->duration <= 0)
        return 0;
    if (tween->elapsed >= tween->duration)
        return 1000;
    double t = (double)tween->elapsed / (double)tween->duration;
    double eased = rt_tween_ease(t, tween->ease_type);
    if (!isfinite(eased))
        return 0;
    int64_t permille = (int64_t)(eased * 1000.0 + 0.5);
    if (permille < 0)
        permille = 0;
    if (permille > 1000)
        permille = 1000;
    return permille;
}

/// @brief Bit-exact integer interpolation at @p t_permille of the way from
///        @p from to @p to.
/// @details Stays in integer arithmetic end to end, so the result is identical
///          on every backend. `t_permille` is clamped to `[0, 1000]`.
/// @param from Start value.
/// @param to End value.
/// @param t_permille Position in thousandths.
/// @return Interpolated value.
int64_t rt_tween_lerp_int_permille(int64_t from, int64_t to, int64_t t_permille) {
    if (t_permille <= 0)
        return from;
    if (t_permille >= 1000)
        return to;
    return from + ((to - from) * t_permille) / 1000;
}

/// @brief True when @p tween was started in millisecond mode.
/// @param tween Borrowed Tween handle.
/// @return Non-zero for a millisecond-mode tween.
int8_t rt_tween_is_ms(rt_tween tween) {
    tween = checked_tween(tween, "Tween.IsMs: expected Zanna.Game.Tween");
    return tween ? tween->ms_mode : 0;
}

/// @brief Enable or disable the reduce-motion snap.
/// @details When enabled, a subsequent Start lands on the target immediately
///          and reports complete, so an accessibility preference needs no
///          special-casing at any call site. Pairs with
///          `Zanna.GUI.ThemePalette.SetMotionEnabled`.
/// @param tween Borrowed Tween handle.
/// @param on Non-zero to snap.
void rt_tween_set_reduce_motion(rt_tween tween, int8_t on) {
    tween = checked_tween(tween, "Tween.ReduceMotion: expected Zanna.Game.Tween");
    if (!tween)
        return;
    tween->reduce_motion = on ? 1 : 0;
}

/// @brief Read the reduce-motion snap flag.
/// @param tween Borrowed Tween handle.
/// @return Non-zero when the snap is enabled.
int8_t rt_tween_get_reduce_motion(rt_tween tween) {
    tween = checked_tween(tween, "Tween.ReduceMotion: expected Zanna.Game.Tween");
    return tween ? tween->reduce_motion : 0;
}
/// @brief Get the current interpolated value as a double.
/// @param tween Borrowed Tween handle.
/// @return Current value, or `0.0` for null.
double rt_tween_value(rt_tween tween) {
    tween = checked_tween(tween, "Tween.Value: expected Zanna.Game.Tween");
    if (!tween)
        return 0.0;
    return tween->current;
}

/// @brief Get the current interpolated value rounded to the nearest integer.
/// @details For tweens started with StartI64 the result is anchored on the exact
/// int64 endpoints: a not-yet-advanced tween returns `from`, a completed tween
/// returns `to`, and a constant tween never drifts — none of which survive the
/// double round-trip that the double-domain fallback uses (VDOC-273).
/// @param tween Borrowed Tween handle.
/// @return Exact/eased integer-domain result for StartI64, otherwise the
///         rounded and saturated current double; `0` for null.
int64_t rt_tween_value_i64(rt_tween tween) {
    tween = checked_tween(tween, "Tween.ValueI64: expected Zanna.Game.Tween");
    if (!tween)
        return 0;

    if (tween->is_i64) {
        if (tween->complete)
            return tween->to_i64;
        if (tween->elapsed <= 0 || tween->duration <= 0)
            return tween->from_i64;
        double t = (double)tween->elapsed / (double)tween->duration;
        if (t > 1.0)
            t = 1.0;
        double eased = rt_tween_ease(t, tween->ease_type);
        return tween_lerp_endpoints_i64(tween->from_i64, tween->to_i64, eased);
    }

    // Double-domain tween: round the interpolated double.
    return tween_round_to_i64(tween->current);
}

/// @brief Check whether the tween is actively interpolating (running and not paused).
/// @param tween Borrowed Tween handle.
/// @return `1` when running and unpaused; otherwise `0`.
int8_t rt_tween_is_running(rt_tween tween) {
    tween = checked_tween(tween, "Tween.IsRunning: expected Zanna.Game.Tween");
    if (!tween)
        return 0;
    return tween->running && !tween->paused;
}

/// @brief Check whether the tween has reached its end value.
/// @param tween Borrowed Tween handle.
/// @return `1` after natural completion until restart/reset; otherwise `0`.
int8_t rt_tween_is_complete(rt_tween tween) {
    tween = checked_tween(tween, "Tween.IsComplete: expected Zanna.Game.Tween");
    if (!tween)
        return 0;
    return tween->complete;
}

/// @brief Get the tween progress as a percentage (0–100).
/// @param tween Borrowed Tween handle.
/// @return Truncated elapsed/duration percentage, or `0` without a duration.
int64_t rt_tween_progress(rt_tween tween) {
    tween = checked_tween(tween, "Tween.Progress: expected Zanna.Game.Tween");
    if (!tween || tween->duration == 0)
        return 0;
    return tween_percent_i64(tween->elapsed, tween->duration);
}

/// @brief Get the number of ticks elapsed since the tween was started.
/// @param tween Borrowed Tween handle.
/// @return Elapsed update frames, or `0` for null.
int64_t rt_tween_elapsed(rt_tween tween) {
    tween = checked_tween(tween, "Tween.Elapsed: expected Zanna.Game.Tween");
    if (!tween)
        return 0;
    return tween->elapsed;
}

/// @brief Get the total duration of the tween in ticks.
/// @param tween Borrowed Tween handle.
/// @return Configured update-frame duration, or `0` for null/unstarted.
int64_t rt_tween_duration(rt_tween tween) {
    tween = checked_tween(tween, "Tween.Duration: expected Zanna.Game.Tween");
    if (!tween)
        return 0;
    return tween->duration;
}

/// @brief Stop the tween and clear the paused state.
/// @details Preserves current value, elapsed frames, endpoints, and completion
///          latch.
/// @param tween Borrowed Tween handle.
void rt_tween_stop(rt_tween tween) {
    tween = checked_tween(tween, "Tween.Stop: expected Zanna.Game.Tween");
    if (!tween)
        return;
    tween->running = 0;
    tween->paused = 0;
}

/// @brief Reset the tween to its start value and restart playback.
/// @details Clears completion/pause, resets elapsed, and resumes only when a
///          positive duration has been configured.
/// @param tween Borrowed Tween handle.
void rt_tween_reset(rt_tween tween) {
    tween = checked_tween(tween, "Tween.Reset: expected Zanna.Game.Tween");
    if (!tween)
        return;
    tween->elapsed = 0;
    tween->current = tween->from;
    tween->complete = 0;
    if (tween->duration > 0)
        tween->running = 1;
    tween->paused = 0;
}

/// @brief Pause the tween at the current position (can be resumed).
/// @param tween Borrowed Tween handle; inactive/completed tweens are unchanged.
void rt_tween_pause(rt_tween tween) {
    tween = checked_tween(tween, "Tween.Pause: expected Zanna.Game.Tween");
    if (!tween)
        return;
    if (tween->running && !tween->complete)
        tween->paused = 1;
}

/// @brief Resume a paused tween from where it left off.
/// @details Clears the pause flag without changing stopped/running state.
/// @param tween Borrowed Tween handle.
void rt_tween_resume(rt_tween tween) {
    tween = checked_tween(tween, "Tween.Resume: expected Zanna.Game.Tween");
    if (!tween)
        return;
    tween->paused = 0;
}

/// @brief Check whether the tween is currently paused.
/// @param tween Borrowed Tween handle.
/// @return `1` when paused; otherwise `0`.
int8_t rt_tween_is_paused(rt_tween tween) {
    tween = checked_tween(tween, "Tween.IsPaused: expected Zanna.Game.Tween");
    if (!tween)
        return 0;
    return tween->paused;
}

//=============================================================================
// Static interpolation functions
//=============================================================================

// Note: rt_lerp is provided by rt_math.c

/// @brief Linearly interpolate between two integers at parameter t, rounded to
/// nearest. Anchored on the integer endpoints so t<=0 returns `from`, t>=1 returns
/// `to`, and a constant is exact — the endpoints no longer round-trip through a
/// double that would drop bits above 2^53 (VDOC-273).
/// @param from Exact integer start endpoint.
/// @param to Exact integer end endpoint.
/// @param t Interpolation parameter; non-finite and nonpositive values select
///        @p from, values at least one select @p to.
/// @return Rounded and saturated interpolated integer.
int64_t rt_tween_lerp_i64(int64_t from, int64_t to, double t) {
    if (!isfinite(t) || t <= 0.0)
        return from;
    if (t >= 1.0)
        return to;
    return tween_lerp_endpoints_i64(from, to, t);
}

/// @brief Apply an easing curve to a linear progress value t in [0,1].
/// @details Dispatches to one of 19 easing functions (linear, quad, cubic, sine,
///          expo, back, bounce — each in in/out/in-out variants). Returns 0 for
///          t<=0 and 1 for t>=1 to guarantee exact endpoints.
/// @param t Linear progress; non-finite input becomes zero.
/// @param ease_type One of the `RT_EASE_*` identifiers; unknown IDs use
///        linear progress.
/// @return Eased progress, potentially outside 0..1 for back curves.
double rt_tween_ease(double t, int64_t ease_type) {
    if (!isfinite(t))
        return 0.0;
    if (t <= 0.0)
        return 0.0;
    if (t >= 1.0)
        return 1.0;

    switch (ease_type) {
        case RT_EASE_LINEAR:
            return ease_linear(t);
        case RT_EASE_IN_QUAD:
            return ease_in_quad(t);
        case RT_EASE_OUT_QUAD:
            return ease_out_quad(t);
        case RT_EASE_IN_OUT_QUAD:
            return ease_in_out_quad(t);
        case RT_EASE_IN_CUBIC:
            return ease_in_cubic(t);
        case RT_EASE_OUT_CUBIC:
            return ease_out_cubic(t);
        case RT_EASE_IN_OUT_CUBIC:
            return ease_in_out_cubic(t);
        case RT_EASE_IN_SINE:
            return ease_in_sine(t);
        case RT_EASE_OUT_SINE:
            return ease_out_sine(t);
        case RT_EASE_IN_OUT_SINE:
            return ease_in_out_sine(t);
        case RT_EASE_IN_EXPO:
            return ease_in_expo(t);
        case RT_EASE_OUT_EXPO:
            return ease_out_expo(t);
        case RT_EASE_IN_OUT_EXPO:
            return ease_in_out_expo(t);
        case RT_EASE_IN_BACK:
            return ease_in_back(t);
        case RT_EASE_OUT_BACK:
            return ease_out_back(t);
        case RT_EASE_IN_OUT_BACK:
            return ease_in_out_back(t);
        case RT_EASE_IN_BOUNCE:
            return ease_in_bounce(t);
        case RT_EASE_OUT_BOUNCE:
            return ease_out_bounce(t);
        case RT_EASE_IN_OUT_BOUNCE:
            return ease_in_out_bounce(t);
        default:
            return t;
    }
}

//=============================================================================
// Internal easing function implementations
//
// Each takes a normalized progress t in [0, 1] and returns the eased value
// (also ~[0, 1], though "back"/"bounce" curves overshoot). Naming follows the
// Penner convention: "in" accelerates from rest, "out" decelerates to rest,
// "in_out" does both around the midpoint.
//=============================================================================

/// @brief Linear (identity) easing: returns @p t unchanged.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return @p t unchanged.
static double ease_linear(double t) {
    return t;
}

/// @brief Quadratic ease-in (t^2).
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Progress that accelerates quadratically from rest.
static double ease_in_quad(double t) {
    return t * t;
}

/// @brief Quadratic ease-out.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Progress that decelerates quadratically toward the endpoint.
static double ease_out_quad(double t) {
    return t * (2.0 - t);
}

/// @brief Quadratic ease-in-out.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Symmetric quadratic acceleration followed by deceleration.
static double ease_in_out_quad(double t) {
    if (t < 0.5)
        return 2.0 * t * t;
    return -1.0 + (4.0 - 2.0 * t) * t;
}

/// @brief Cubic ease-in (t^3).
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Progress that accelerates cubically from rest.
static double ease_in_cubic(double t) {
    return t * t * t;
}

/// @brief Cubic ease-out.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Progress that decelerates cubically toward the endpoint.
static double ease_out_cubic(double t) {
    double t1 = t - 1.0;
    return t1 * t1 * t1 + 1.0;
}

/// @brief Cubic ease-in-out.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Symmetric cubic acceleration followed by deceleration.
static double ease_in_out_cubic(double t) {
    if (t < 0.5)
        return 4.0 * t * t * t;
    double t1 = 2.0 * t - 2.0;
    return 0.5 * t1 * t1 * t1 + 1.0;
}

/// @brief Sinusoidal ease-in (1 - cos).
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Quarter-cosine progress that starts with zero slope.
static double ease_in_sine(double t) {
    return 1.0 - cos(t * M_PI / 2.0);
}

/// @brief Sinusoidal ease-out (sin).
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Quarter-sine progress that ends with zero slope.
static double ease_out_sine(double t) {
    return sin(t * M_PI / 2.0);
}

/// @brief Sinusoidal ease-in-out.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Half-cosine progress with zero slope at both endpoints.
static double ease_in_out_sine(double t) {
    return 0.5 * (1.0 - cos(M_PI * t));
}

/// @brief Exponential ease-in (2^(10(t-1))).
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Exponentially accelerating progress with an exact zero endpoint.
static double ease_in_expo(double t) {
    if (t == 0.0)
        return 0.0;
    return pow(2.0, 10.0 * (t - 1.0));
}

/// @brief Exponential ease-out.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Exponentially decelerating progress with an exact one endpoint.
static double ease_out_expo(double t) {
    if (t == 1.0)
        return 1.0;
    return 1.0 - pow(2.0, -10.0 * t);
}

/// @brief Exponential ease-in-out.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Symmetric exponential progress with exact zero and one endpoints.
static double ease_in_out_expo(double t) {
    if (t == 0.0)
        return 0.0;
    if (t == 1.0)
        return 1.0;
    if (t < 0.5)
        return 0.5 * pow(2.0, 20.0 * t - 10.0);
    return 1.0 - 0.5 * pow(2.0, -20.0 * t + 10.0);
}

/// @brief "Back" ease-in: slight anticipation (undershoot) before advancing.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Progress that initially falls below zero before reaching one.
static double ease_in_back(double t) {
    const double c1 = 1.70158;
    const double c3 = c1 + 1.0;
    return c3 * t * t * t - c1 * t * t;
}

/// @brief "Back" ease-out: overshoots the target then settles back.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Progress that exceeds one before settling at the endpoint.
static double ease_out_back(double t) {
    const double c1 = 1.70158;
    const double c3 = c1 + 1.0;
    double t1 = t - 1.0;
    return 1.0 + c3 * t1 * t1 * t1 + c1 * t1 * t1;
}

/// @brief "Back" ease-in-out: anticipation at the start, overshoot at the end.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Symmetric back progress that may fall below zero and exceed one.
static double ease_in_out_back(double t) {
    const double c1 = 1.70158;
    const double c2 = c1 * 1.525;
    if (t < 0.5) {
        double t2 = 2.0 * t;
        return 0.5 * t2 * t2 * ((c2 + 1.0) * t2 - c2);
    }
    double t2 = 2.0 * t - 2.0;
    return 0.5 * (t2 * t2 * ((c2 + 1.0) * t2 + c2) + 2.0);
}

/// @brief "Bounce" ease-out: decaying piecewise-parabolic bounces to the end.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Piecewise parabolic progress that approaches one through rebounds.
static double ease_out_bounce(double t) {
    const double n1 = 7.5625;
    const double d1 = 2.75;

    if (t < 1.0 / d1) {
        return n1 * t * t;
    } else if (t < 2.0 / d1) {
        double t1 = t - 1.5 / d1;
        return n1 * t1 * t1 + 0.75;
    } else if (t < 2.5 / d1) {
        double t1 = t - 2.25 / d1;
        return n1 * t1 * t1 + 0.9375;
    } else {
        double t1 = t - 2.625 / d1;
        return n1 * t1 * t1 + 0.984375;
    }
}

/// @brief "Bounce" ease-in: time-reversed ease_out_bounce.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Reversed bounce progress that rebounds before accelerating forward.
static double ease_in_bounce(double t) {
    return 1.0 - ease_out_bounce(1.0 - t);
}

/// @brief "Bounce" ease-in-out: bounce-in for the first half, bounce-out the second.
/// @param t Normalized linear progress in the inclusive range 0..1.
/// @return Symmetric bounce-in/bounce-out progress.
static double ease_in_out_bounce(double t) {
    if (t < 0.5)
        return 0.5 * (1.0 - ease_out_bounce(1.0 - 2.0 * t));
    return 0.5 * (1.0 + ease_out_bounce(2.0 * t - 1.0));
}
