//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_screenfx.c
/// @file
/// @brief Implements composable screen shake, color overlays, and Canvas-drawn
///        transition effects.
// Purpose: Screen effects manager for Zanna games. Provides camera shake,
//   color flash, fade-in, and fade-out effects that are composited each frame.
//   Effects are stored in a growable slot array and updated with a delta-time
//   value (milliseconds). Multiple effects of different types can run
//   simultaneously; shake offsets are accumulated and the brightest overlay
//   alpha wins (max-alpha compositing).
//
// Key invariants:
//   - The initial reservation is RT_SCREENFX_MAX_EFFECTS slots. New effects
//     grow the slot array on demand instead of being silently dropped.
//   - Shake: uses per-instance LCG RNG seeded from the active runtime RNG, so
//     RANDOMIZE controls reproducibility without exposing object addresses.
//   - Shake decay model: decay parameter controls the exponent applied to the
//     remaining-time factor (1 - progress):
//       decay == 0      → no decay (constant amplitude throughout)
//       decay == 1000   → linear decay: amplitude ∝ (1 - t)
//       decay >= 1500   → quadratic decay: amplitude ∝ (1 - t)²
//     The quadratic "trauma model" feels more natural for game camera shake.
//   - Color format for all effects is 0xRRGGBBAA (32-bit, alpha in low byte).
//     This differs from the Canvas drawing API which uses 0x00RRGGBB. Pass
//     colors through the ScreenFX API using this format, not Canvas colors.
//   - Flash alpha fades from base_alpha → 0 over the duration (starts bright).
//   - FadeIn alpha fades from base_alpha → 0 (fades FROM the color TO clear).
//   - FadeOut alpha fades from 0 → base_alpha (fades FROM clear TO the color).
//   - Starting a new FadeIn or FadeOut cancels any currently running fade.
//
// Ownership/Lifetime:
//   - ScreenFX objects are GC-managed via rt_obj_new_i64. rt_screenfx_destroy()
//     calls rt_obj_free() for callers that manage lifetimes explicitly, but GC
//     finalisation also reclaims the allocation automatically.
//
// Links: src/runtime/game/rt_screenfx.h (public API),
//        docs/zannalib/game.md (ScreenFX section, color format table)
//
//===----------------------------------------------------------------------===//

#include "rt_screenfx.h"
#include "rt_object.h"
#include "rt_random.h"
#include "rt_trap.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Advance a per-instance linear congruential generator.
/// @details Uses the classic `glibc` parameters (1103515245, 12345) and
///          discards the low 16 bits. Per-object state keeps concurrent
///          ScreenFX instances independent without a shared lock.
/// @param state Mutable 64-bit generator state.
/// @return Next pseudo-random value in the inclusive range 0..32767.
static int64_t screenfx_rand(uint64_t *state) {
    *state = (*state) * UINT64_C(1103515245) + UINT64_C(12345);
    return (int64_t)((*state >> 16) & UINT64_C(0x7FFF));
}

/// @brief Storage shared by every supported effect type.
struct screenfx_effect {
    rt_screenfx_type_t type; ///< Effect type.
    int64_t color;           ///< Color (RGBA for old effects, RGB for transitions).
    int64_t intensity;       ///< Intensity (shake) / direction (wipe) / max_block (pixelate).
    int64_t duration;        ///< Total duration (ms).
    int64_t elapsed;         ///< Elapsed time (ms).
    int64_t decay;           ///< Decay rate (shake) / center_x (circle).
    int64_t extra;           ///< center_y (circle) / unused for others.
    int8_t terminal;         ///< 1 once the effect has reached its final (progress=1000)
                             ///< frame. The slot stays drawable for that one frame and is
                             ///< reclaimed on the next update, so the fully-covered final
                             ///< frame is observable to Draw/TransitionProgress (VDOC-265).
};

/// @brief ScreenFX object's growable slots and cached composite output.
struct rt_screenfx_impl {
    struct screenfx_effect *effects;
    int64_t effect_capacity;
    int64_t shake_x;       ///< Current shake offset X.
    int64_t shake_y;       ///< Current shake offset Y.
    int64_t overlay_color; ///< Current overlay color (RGB).
    int64_t overlay_alpha; ///< Current overlay alpha (0-255).
    uint64_t rand_state;   ///< Per-instance LCG state for shake RNG (thread-safe).
};

/// @brief Safe-cast a handle to the ScreenFX impl, trapping @p api on a
///        class-id mismatch.
/// @param fx Borrowed candidate ScreenFX handle.
/// @param api Trap message identifying the calling API.
/// @return Borrowed implementation pointer, or `NULL` when @p fx is `NULL`.
static rt_screenfx checked_screenfx(rt_screenfx fx, const char *api) {
    if (!fx)
        return NULL;
    if (rt_obj_class_id(fx) != RT_SCREENFX_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return fx;
}

/// @brief Add a non-negative @p b to @p a, saturating at INT64_MAX
///        (negative @p b is treated as 0).
/// @param a Starting signed value.
/// @param b Non-negative increment; negative values are ignored.
/// @return Saturated sum.
static int64_t add_sat_nonnegative_i64(int64_t a, int64_t b) {
    if (b <= 0)
        return a;
    return a > INT64_MAX - b ? INT64_MAX : a + b;
}

/// @brief Saturating int64 addition (clamps to INT64_MIN/MAX on overflow).
/// @param a First addend.
/// @param b Second addend.
/// @return Exact sum when representable, otherwise the matching signed bound.
static int64_t add_sat_i64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Convert a long double to int64, saturating to the int64 range.
/// @details Non-finite -> INT64_MIN/MAX by sign. The platforms whose long
///          double has < 64-bit mantissa back the bound off by 4096 so the
///          comparison itself can't round past the representable limit.
/// @param value Extended-precision value to convert.
/// @return Truncated value clamped to the signed 64-bit range.
static int64_t ld_to_i64_sat(long double value) {
    if (!isfinite(value))
        return value < 0.0L ? INT64_MIN : INT64_MAX;
#if LDBL_MANT_DIG < 64
    if (value >= (long double)(INT64_MAX - INT64_C(4096)))
        return INT64_MAX;
    if (value <= (long double)(INT64_MIN + INT64_C(4096)))
        return INT64_MIN;
#else
    if (value > (long double)INT64_MAX)
        return INT64_MAX;
    if (value < (long double)INT64_MIN)
        return INT64_MIN;
#endif
    return (int64_t)value;
}

/// @brief Compute (a*b)/divisor in long double, saturating to int64; 0 when
///        @p divisor is 0.
/// @param a First multiplicand.
/// @param b Second multiplicand.
/// @param divisor Divisor, which may be zero.
/// @return Saturated quotient, or `0` for a zero divisor.
static int64_t mul_div_sat_i64(int64_t a, int64_t b, int64_t divisor) {
    if (divisor == 0)
        return 0;
    long double value = ((long double)a * (long double)b) / (long double)divisor;
    return ld_to_i64_sat(value);
}

/// @brief Ensure the manager has at least a requested number of effect slots.
/// @details Capacity doubles geometrically and new slots are zero-initialized.
///          Integer and allocation-size overflow are rejected before realloc.
/// @param fx Borrowed ScreenFX implementation.
/// @param needed Minimum required slot count.
/// @return `1` when capacity is sufficient; `0` on overflow or allocation
///         failure.
static int8_t ensure_effect_capacity(rt_screenfx fx, int64_t needed) {
    if (!fx || needed <= fx->effect_capacity)
        return 1;
    int64_t new_capacity = fx->effect_capacity > 0 ? fx->effect_capacity : 1;
    while (new_capacity < needed) {
        if (new_capacity > INT64_MAX / 2 ||
            (uint64_t)new_capacity > SIZE_MAX / (2 * sizeof(struct screenfx_effect)))
            return 0;
        new_capacity *= 2;
    }
    if ((uint64_t)new_capacity > SIZE_MAX / sizeof(struct screenfx_effect))
        return 0;
    struct screenfx_effect *resized = (struct screenfx_effect *)realloc(
        fx->effects, (size_t)new_capacity * sizeof(struct screenfx_effect));
    if (!resized)
        return 0;
    memset(resized + fx->effect_capacity,
           0,
           (size_t)(new_capacity - fx->effect_capacity) * sizeof(struct screenfx_effect));
    fx->effects = resized;
    fx->effect_capacity = new_capacity;
    return 1;
}

/// @brief Effect progress as per-mille (0..1000) of elapsed/duration; returns
///        1000 (complete) for a zero/negative duration or NULL effect.
/// @param e Borrowed effect slot.
/// @return Clamped progress in the inclusive range 0..1000.
static int64_t effect_progress_per_mille(const struct screenfx_effect *e) {
    if (!e || e->duration <= 0)
        return 1000;
    long double progress = ((long double)e->elapsed * 1000.0L) / (long double)e->duration;
    if (progress <= 0.0L)
        return 0;
    if (progress >= 1000.0L)
        return 1000;
    return (int64_t)progress;
}

/// @brief Floor integer square root of a non-negative value. Seeds from the
///        floating-point sqrt then corrects any rounding drift so the result is
///        exact for all inputs.
/// @param n Non-negative radicand; nonpositive values return zero.
/// @return Greatest integer whose square does not exceed @p n.
static int64_t screenfx_isqrt(int64_t n) {
    if (n <= 0)
        return 0;
    int64_t x = (int64_t)sqrtl((long double)n);
    while (x > 0 && x * x > n)
        x--;
    while ((x + 1) * (x + 1) <= n)
        x++;
    return x;
}

/// @brief Horizontal half-chord of a disc of the given @p radius at vertical
///        offset @p dy from the center: the half-width of the circular opening
///        on that scanline, or 0 when the row lies entirely outside the disc.
/// Exposed (non-static) so tests can verify the mask is genuinely circular —
/// a diagonal point outside the disc but inside its bounding square must fall
/// beyond the half-chord (VDOC-269). Internal symbol; not a registered API.
/// @param radius Disc radius.
/// @param dy Signed vertical distance from the center.
/// @return Non-negative horizontal half-chord, or `0` outside/at the boundary.
int64_t rt_screenfx_circle_half_chord(int64_t radius, int64_t dy) {
    if (radius <= 0)
        return 0;
    int64_t inside = radius * radius - dy * dy;
    if (inside <= 0)
        return 0;
    return screenfx_isqrt(inside);
}

/// @brief Release heap-owned effect slots during runtime-object finalization.
/// @param obj Borrowed ScreenFX object being finalized.
static void screenfx_finalizer(void *obj) {
    rt_screenfx fx = (rt_screenfx)obj;
    if (!fx)
        return;
    free(fx->effects);
    fx->effects = NULL;
    fx->effect_capacity = 0;
}

// Effect-type constants — return the private enum values as stable public
// identifiers so callers of IsTypeActive/CancelType never copy raw integers.
/// @brief Return the stable camera-shake effect identifier.
/// @return @ref RT_SCREENFX_SHAKE.
int64_t rt_screenfx_type_shake(void) { return RT_SCREENFX_SHAKE; }
/// @brief Return the stable color-flash effect identifier.
/// @return @ref RT_SCREENFX_FLASH.
int64_t rt_screenfx_type_flash(void) { return RT_SCREENFX_FLASH; }
/// @brief Return the stable fade-in effect identifier.
/// @return @ref RT_SCREENFX_FADE_IN.
int64_t rt_screenfx_type_fade_in(void) { return RT_SCREENFX_FADE_IN; }
/// @brief Return the stable fade-out effect identifier.
/// @return @ref RT_SCREENFX_FADE_OUT.
int64_t rt_screenfx_type_fade_out(void) { return RT_SCREENFX_FADE_OUT; }
/// @brief Return the stable directional-wipe effect identifier.
/// @return @ref RT_SCREENFX_WIPE.
int64_t rt_screenfx_type_wipe(void) { return RT_SCREENFX_WIPE; }
/// @brief Return the stable closing-circle effect identifier.
/// @return @ref RT_SCREENFX_CIRCLE_IN.
int64_t rt_screenfx_type_circle_in(void) { return RT_SCREENFX_CIRCLE_IN; }
/// @brief Return the stable opening-circle effect identifier.
/// @return @ref RT_SCREENFX_CIRCLE_OUT.
int64_t rt_screenfx_type_circle_out(void) { return RT_SCREENFX_CIRCLE_OUT; }
/// @brief Return the stable ordered-dissolve effect identifier.
/// @return @ref RT_SCREENFX_DISSOLVE.
int64_t rt_screenfx_type_dissolve(void) { return RT_SCREENFX_DISSOLVE; }
/// @brief Return the stable pixelate-grid effect identifier.
/// @return @ref RT_SCREENFX_PIXELATE.
int64_t rt_screenfx_type_pixelate(void) { return RT_SCREENFX_PIXELATE; }

// Wipe-direction constants.
/// @brief Return the left-origin wipe direction identifier.
/// @return @ref RT_DIR_LEFT.
int64_t rt_screenfx_dir_left(void) { return RT_DIR_LEFT; }
/// @brief Return the right-origin wipe direction identifier.
/// @return @ref RT_DIR_RIGHT.
int64_t rt_screenfx_dir_right(void) { return RT_DIR_RIGHT; }
/// @brief Return the top-origin wipe direction identifier.
/// @return @ref RT_DIR_UP.
int64_t rt_screenfx_dir_up(void) { return RT_DIR_UP; }
/// @brief Return the bottom-origin wipe direction identifier.
/// @return @ref RT_DIR_DOWN.
int64_t rt_screenfx_dir_down(void) { return RT_DIR_DOWN; }

/// @brief Clamp @p v to a single 0-255 color channel.
/// @param v Signed channel value.
/// @return Unsigned channel value in the inclusive range 0..255.
static uint32_t clamp_channel(int64_t v) {
    return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/// @brief Pack a Flash/FadeIn/FadeOut overlay color as 0xRRGGBBAA (alpha low byte).
/// This is deliberately a different byte order from the canonical
/// Zanna.Graphics.Color (0xAARRGGBB) — see the header note.
/// @param r Red channel.
/// @param g Green channel.
/// @param b Blue channel.
/// @param a Alpha channel.
/// @return Packed color after independently clamping every channel.
int64_t rt_screenfx_rgba(int64_t r, int64_t g, int64_t b, int64_t a) {
    return (int64_t)((clamp_channel(r) << 24) | (clamp_channel(g) << 16) |
                     (clamp_channel(b) << 8) | clamp_channel(a));
}

/// @brief Pack a transition color as 0x00RRGGBB (Canvas byte order, no alpha).
/// @param r Red channel.
/// @param g Green channel.
/// @param b Blue channel.
/// @return Packed Canvas color after independently clamping every channel.
int64_t rt_screenfx_rgb(int64_t r, int64_t g, int64_t b) {
    return (int64_t)((clamp_channel(r) << 16) | (clamp_channel(g) << 8) | clamp_channel(b));
}

/// @brief Construct an empty ScreenFX manager.
/// @details RNG state is seeded from the active runtime RNG so callers can
///          reproduce effects through RANDOMIZE without relying on process
///          address layout. Returns a GC-managed handle; NULL on allocation
///          failure.
/// @return Owned ScreenFX handle, or `NULL` if object or slot allocation fails.
rt_screenfx rt_screenfx_new(void) {
    struct rt_screenfx_impl *fx = (struct rt_screenfx_impl *)rt_obj_new_i64(
        RT_SCREENFX_CLASS_ID, (int64_t)sizeof(struct rt_screenfx_impl));
    if (!fx)
        return NULL;

    memset(fx, 0, sizeof(struct rt_screenfx_impl));
    fx->effect_capacity = RT_SCREENFX_MAX_EFFECTS;
    fx->effects = (struct screenfx_effect *)calloc((size_t)fx->effect_capacity,
                                                   sizeof(struct screenfx_effect));
    if (!fx->effects) {
        if (rt_obj_release_check0(fx))
            rt_obj_free(fx);
        return NULL;
    }
    fx->rand_state = ((uint64_t)rt_rand_range(0, LLONG_MAX) << 1) ^
                     (uint64_t)rt_rand_range(0, LLONG_MAX) ^ UINT64_C(0xDEADBEEF);
    if (fx->rand_state == 0)
        fx->rand_state = UINT64_C(0xDEADBEEF);
    rt_obj_set_finalizer(fx, screenfx_finalizer);
    return fx;
}

/// @brief Release the ScreenFX manager; frees the inline struct when refcount hits zero.
/// Provided for API symmetry — GC finalization also reclaims the allocation automatically.
/// @param fx Owned ScreenFX reference to release; `NULL` is ignored.
void rt_screenfx_destroy(rt_screenfx fx) {
    fx = checked_screenfx(fx, "ScreenFX.Destroy: expected Zanna.Game.ScreenFX");
    if (fx && rt_obj_release_check0(fx))
        rt_obj_free(fx);
}

/// @brief Linear scan for the first free slot, growing the slot array if needed.
/// @param fx Borrowed ScreenFX implementation.
/// @return Free zero-based slot index, or `-1` if growth fails.
static int64_t find_free_slot(rt_screenfx fx) {
    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        if (fx->effects[i].type == RT_SCREENFX_NONE)
            return i;
    }
    int64_t slot = fx->effect_capacity;
    if (ensure_effect_capacity(fx, slot + 1))
        return slot;
    return -1;
}

/// @brief Locate an existing effect of the given type so it can be reused/restarted (e.g. shake
/// only ever lives in one slot — re-triggering replaces in place rather than allocating).
/// @param fx Borrowed ScreenFX implementation.
/// @param type Effect type to locate.
/// @return First matching slot index, or `-1` when absent.
static int64_t find_effect_of_type(rt_screenfx fx, rt_screenfx_type_t type) {
    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        if (fx->effects[i].type == type)
            return i;
    }
    return -1;
}

/// @brief Recompute the composited shake offset and overlay color/alpha from the CURRENT
/// (already-advanced) state of every live slot, without advancing time. Behavior per type:
///   - **SHAKE:** decay-modulated random offset accumulated into shake_x/shake_y. Decay model:
///     `decay <= 0` constant amplitude; `decay >= 1500` quadratic (trauma); else linear.
///   - **FLASH / FADE_IN:** alpha fades from `base_alpha → 0` over the duration.
///   - **FADE_OUT:** alpha fades from `0 → base_alpha`.
///   - **WIPE / CIRCLE_* / DISSOLVE / PIXELATE:** contribute nothing here; rendered in `draw()`.
/// Multiple overlays use **max-alpha compositing** — the brightest active overlay wins.
///
/// Splitting this out of the update tick lets cancellation paths rebuild the cached draw state
/// from the surviving slots, so a canceled flash/fade/shake can never keep drawing stale output
/// after its slot is gone (VDOC-266).
/// @param fx Borrowed ScreenFX implementation whose caches are replaced.
static void screenfx_recompute(rt_screenfx fx) {
    fx->shake_x = 0;
    fx->shake_y = 0;
    fx->overlay_alpha = 0;
    fx->overlay_color = 0;

    int64_t max_overlay_alpha = 0;

    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        struct screenfx_effect *e = &fx->effects[i];
        if (e->type == RT_SCREENFX_NONE)
            continue;

        // Calculate progress (0-1000)
        int64_t progress = effect_progress_per_mille(e);

        switch (e->type) {
            case RT_SCREENFX_SHAKE: {
                // Exponential decay: intensity falls as (1 - progress/1000)^decay_exp
                // where decay_exp is controlled by e->decay (higher = faster decay).
                // When decay == 0 → no decay (constant intensity).
                // When decay == 1000 → approximately linear decay.
                // When decay == 2000 → quadratic (trauma model: natural feel).
                int64_t current_intensity;
                if (e->decay <= 0) {
                    current_intensity = e->intensity;
                } else {
                    // Use integer approximation of (1 - t)^2 for decay==2000 default
                    // General form: factor = (1000 - progress)^(decay/1000) / 1000
                    int64_t remaining = 1000 - progress; // 0..1000
                    if (remaining < 0)
                        remaining = 0;
                    // decay stored as 1000×exponent; apply once for linear, twice for quadratic
                    int64_t decay_factor = remaining; // always at least one factor
                    if (e->decay >= 1500)             // >= 1.5 exponent → apply twice
                        decay_factor = (remaining * remaining) / 1000;
                    current_intensity = mul_div_sat_i64(e->intensity, decay_factor, 1000);
                }

                // Random offset based on intensity (per-instance state)
                int64_t rx = (screenfx_rand(&fx->rand_state) % 2001) - 1000;
                int64_t ry = (screenfx_rand(&fx->rand_state) % 2001) - 1000;
                fx->shake_x =
                    add_sat_i64(fx->shake_x, mul_div_sat_i64(current_intensity, rx, 1000));
                fx->shake_y =
                    add_sat_i64(fx->shake_y, mul_div_sat_i64(current_intensity, ry, 1000));
                break;
            }

            case RT_SCREENFX_FLASH: {
                // Flash starts bright and fades
                // Color format: 0xRRGGBBAA — alpha in low byte
                int64_t alpha = ((e->color & 0xFF) * (1000 - progress)) / 1000;
                if (alpha > max_overlay_alpha) {
                    max_overlay_alpha = alpha;
                    fx->overlay_color = e->color & 0xFFFFFF00;
                    fx->overlay_alpha = alpha;
                }
                break;
            }

            case RT_SCREENFX_FADE_IN: {
                // Fade from color to clear
                int64_t base_alpha = e->color & 0xFF;
                int64_t alpha = (base_alpha * (1000 - progress)) / 1000;
                if (alpha > max_overlay_alpha) {
                    max_overlay_alpha = alpha;
                    fx->overlay_color = e->color & 0xFFFFFF00;
                    fx->overlay_alpha = alpha;
                }
                break;
            }

            case RT_SCREENFX_FADE_OUT: {
                // Fade from clear to color
                int64_t base_alpha = e->color & 0xFF;
                int64_t alpha = (base_alpha * progress) / 1000;
                if (alpha > max_overlay_alpha) {
                    max_overlay_alpha = alpha;
                    fx->overlay_color = e->color & 0xFFFFFF00;
                    fx->overlay_alpha = alpha;
                }
                break;
            }

            case RT_SCREENFX_WIPE:
            case RT_SCREENFX_CIRCLE_IN:
            case RT_SCREENFX_CIRCLE_OUT:
            case RT_SCREENFX_DISSOLVE:
            case RT_SCREENFX_PIXELATE:
                // Transitions are rendered via Draw(); they contribute no overlay here.
                break;

            default:
                break;
        }
    }
}

/// @brief Per-frame tick: advance every live effect by `dt` ms, then recompose the shake offset
/// and overlay color/alpha. An effect that reaches its duration this tick is NOT reclaimed
/// immediately; it is clamped to its exact end (progress = 1000) and marked @c terminal so its
/// fully-covered final frame is composited and stays drawable for one more Draw. The slot is
/// reclaimed on the following update. This separates "finished advancing" from "removed" so a
/// covering fade/wipe cannot vanish before its last frame is observed (VDOC-265).
/// @param fx Borrowed ScreenFX handle.
/// @param dt Positive elapsed time in milliseconds; nonpositive values are
///        ignored without recomputing cached output.
void rt_screenfx_update(rt_screenfx fx, int64_t dt) {
    fx = checked_screenfx(fx, "ScreenFX.Update: expected Zanna.Game.ScreenFX");
    if (!fx || dt <= 0)
        return;

    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        struct screenfx_effect *e = &fx->effects[i];
        if (e->type == RT_SCREENFX_NONE)
            continue;

        // A slot that already showed its terminal frame last tick is reclaimed now.
        if (e->terminal) {
            e->type = RT_SCREENFX_NONE;
            e->terminal = 0;
            continue;
        }

        e->elapsed = add_sat_nonnegative_i64(e->elapsed, dt);

        // On reaching the end, clamp to the exact duration so progress reads 1000, and defer
        // removal by one tick so the final fully-covered frame is drawn.
        if (e->elapsed >= e->duration) {
            e->elapsed = e->duration;
            e->terminal = 1;
        }
    }

    screenfx_recompute(fx);
}

/// @brief Trigger a camera shake: random per-frame offset of magnitude `intensity` (pixels),
/// damped over `duration` ms by the `decay` model (0 = constant, 1000 = linear, ≥1500 = quadratic).
/// Replaces any existing shake in place — only one shake runs at a time.
/// @param fx Borrowed ScreenFX handle.
/// @param intensity Maximum per-axis offset magnitude in pixels; negatives
///        clamp to zero.
/// @param duration Positive effect duration in milliseconds.
/// @param decay Per-mille decay-model selector.
void rt_screenfx_shake(rt_screenfx fx, int64_t intensity, int64_t duration, int64_t decay) {
    fx = checked_screenfx(fx, "ScreenFX.Shake: expected Zanna.Game.ScreenFX");
    if (!fx || duration <= 0)
        return;
    if (intensity < 0)
        intensity = 0;

    // Find existing shake or free slot
    int64_t slot = find_effect_of_type(fx, RT_SCREENFX_SHAKE);
    if (slot < 0)
        slot = find_free_slot(fx);
    if (slot < 0)
        return;

    struct screenfx_effect *e = &fx->effects[slot];
    e->type = RT_SCREENFX_SHAKE;
    e->intensity = intensity;
    e->duration = duration;
    e->elapsed = 0;
    e->decay = decay;
    e->color = 0;
    e->terminal = 0; // reused shake slot may still carry last cycle's terminal flag
}

/// @brief Trigger a one-shot color flash. `color` is 0xRRGGBBAA (alpha encodes peak intensity);
/// alpha fades from peak to 0 over `duration` ms. Useful for hit-frames, lightning, screen blasts.
/// Multiple simultaneous flashes pick the one with the highest current alpha.
/// @param fx Borrowed ScreenFX handle.
/// @param color Packed 0xRRGGBBAA peak overlay color.
/// @param duration Positive effect duration in milliseconds.
void rt_screenfx_flash(rt_screenfx fx, int64_t color, int64_t duration) {
    fx = checked_screenfx(fx, "ScreenFX.Flash: expected Zanna.Game.ScreenFX");
    if (!fx || duration <= 0)
        return;

    int64_t slot = find_free_slot(fx);
    if (slot < 0)
        return;

    struct screenfx_effect *e = &fx->effects[slot];
    e->type = RT_SCREENFX_FLASH;
    e->color = color;
    e->duration = duration;
    e->elapsed = 0;
    e->intensity = 0;
    e->decay = 0;
}

/// @brief Fade FROM the color TO clear (alpha goes peak→0). Used at scene-entry — start covered,
/// reveal underlying gameplay. Cancels any in-flight FADE_IN/FADE_OUT first so fades don't stack.
/// @param fx Borrowed ScreenFX handle.
/// @param color Packed 0xRRGGBBAA starting overlay color.
/// @param duration Positive effect duration in milliseconds.
void rt_screenfx_fade_in(rt_screenfx fx, int64_t color, int64_t duration) {
    fx = checked_screenfx(fx, "ScreenFX.FadeIn: expected Zanna.Game.ScreenFX");
    if (!fx || duration <= 0)
        return;

    // Cancel existing fades
    rt_screenfx_cancel_type(fx, RT_SCREENFX_FADE_IN);
    rt_screenfx_cancel_type(fx, RT_SCREENFX_FADE_OUT);

    int64_t slot = find_free_slot(fx);
    if (slot < 0)
        return;

    struct screenfx_effect *e = &fx->effects[slot];
    e->type = RT_SCREENFX_FADE_IN;
    e->color = color;
    e->duration = duration;
    e->elapsed = 0;
    e->intensity = 0;
    e->decay = 0;
}

/// @brief Fade FROM clear TO the color (alpha goes 0→peak). Used at scene-exit — start clear,
/// end covered. Cancels any in-flight FADE_IN/FADE_OUT first.
/// @param fx Borrowed ScreenFX handle.
/// @param color Packed 0xRRGGBBAA final overlay color.
/// @param duration Positive effect duration in milliseconds.
void rt_screenfx_fade_out(rt_screenfx fx, int64_t color, int64_t duration) {
    fx = checked_screenfx(fx, "ScreenFX.FadeOut: expected Zanna.Game.ScreenFX");
    if (!fx || duration <= 0)
        return;

    // Cancel existing fades
    rt_screenfx_cancel_type(fx, RT_SCREENFX_FADE_IN);
    rt_screenfx_cancel_type(fx, RT_SCREENFX_FADE_OUT);

    int64_t slot = find_free_slot(fx);
    if (slot < 0)
        return;

    struct screenfx_effect *e = &fx->effects[slot];
    e->type = RT_SCREENFX_FADE_OUT;
    e->color = color;
    e->duration = duration;
    e->elapsed = 0;
    e->intensity = 0;
    e->decay = 0;
}

/// @brief Stop every active effect immediately and zero the composited shake/overlay state.
/// Use between scene transitions to guarantee a clean slate.
/// @param fx Borrowed ScreenFX handle.
void rt_screenfx_cancel_all(rt_screenfx fx) {
    fx = checked_screenfx(fx, "ScreenFX.CancelAll: expected Zanna.Game.ScreenFX");
    if (!fx)
        return;

    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        fx->effects[i].type = RT_SCREENFX_NONE;
        fx->effects[i].terminal = 0;
    }

    fx->shake_x = 0;
    fx->shake_y = 0;
    fx->overlay_color = 0;
    fx->overlay_alpha = 0;
}

/// @brief Stop every effect of a single type (e.g. cancel all FADE_IN slots before issuing a new
/// one). The composited shake/overlay state is rebuilt from the surviving slots so a canceled
/// flash/fade cannot keep drawing its cached overlay and a canceled shake cannot leave a stale
/// camera offset (VDOC-266). When nothing of that type was active the recompute is a cheap no-op
/// over the same slots the caller would have scanned anyway.
/// @param fx Borrowed ScreenFX handle.
/// @param type Stable effect-type identifier to cancel.
void rt_screenfx_cancel_type(rt_screenfx fx, int64_t type) {
    fx = checked_screenfx(fx, "ScreenFX.CancelType: expected Zanna.Game.ScreenFX");
    if (!fx)
        return;

    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        if (fx->effects[i].type == (rt_screenfx_type_t)type) {
            fx->effects[i].type = RT_SCREENFX_NONE;
            fx->effects[i].terminal = 0;
        }
    }

    screenfx_recompute(fx);
}

/// @brief Returns 1 if any effect slot is currently in use.
/// @param fx Borrowed ScreenFX handle.
/// @return `1` when any effect, including a terminal-frame effect, occupies a
///         slot; otherwise `0`.
int8_t rt_screenfx_is_active(rt_screenfx fx) {
    fx = checked_screenfx(fx, "ScreenFX.IsActive: expected Zanna.Game.ScreenFX");
    if (!fx)
        return 0;

    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        if (fx->effects[i].type != RT_SCREENFX_NONE)
            return 1;
    }
    return 0;
}

/// @brief Returns 1 if at least one slot of the given effect type is active. Useful for guarding
/// "don't restart this effect if it's still running" patterns.
/// @param fx Borrowed ScreenFX handle.
/// @param type Stable effect-type identifier to query.
/// @return `1` when a matching slot exists; otherwise `0`.
int8_t rt_screenfx_is_type_active(rt_screenfx fx, int64_t type) {
    fx = checked_screenfx(fx, "ScreenFX.IsTypeActive: expected Zanna.Game.ScreenFX");
    if (!fx)
        return 0;

    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        if (fx->effects[i].type == (rt_screenfx_type_t)type)
            return 1;
    }
    return 0;
}

/// @brief Read the composited X-axis camera-shake offset for the current frame. Caller adds this
/// to the camera position before drawing the world.
/// @param fx Borrowed ScreenFX handle.
/// @return Current accumulated horizontal offset in pixels, or `0` for null.
int64_t rt_screenfx_get_shake_x(rt_screenfx fx) {
    fx = checked_screenfx(fx, "ScreenFX.ShakeX: expected Zanna.Game.ScreenFX");
    return fx ? fx->shake_x : 0;
}

/// @brief Read the composited Y-axis camera-shake offset for the current frame.
/// @param fx Borrowed ScreenFX handle.
/// @return Current accumulated vertical offset in pixels, or `0` for null.
int64_t rt_screenfx_get_shake_y(rt_screenfx fx) {
    fx = checked_screenfx(fx, "ScreenFX.ShakeY: expected Zanna.Game.ScreenFX");
    return fx ? fx->shake_y : 0;
}

/// @brief Read the current overlay color (RGB packed in upper 24 bits, alpha in low byte = 0).
/// Use together with `get_overlay_alpha` to render a full-screen tinted box on top of gameplay.
/// @param fx Borrowed ScreenFX handle.
/// @return Packed 0xRRGGBB00 overlay color, or `0` when unavailable.
int64_t rt_screenfx_get_overlay_color(rt_screenfx fx) {
    fx = checked_screenfx(fx, "ScreenFX.OverlayColor: expected Zanna.Game.ScreenFX");
    return fx ? fx->overlay_color : 0;
}

/// @brief Read the current overlay alpha (0–255). Effects use max-alpha compositing — the
/// brightest active overlay (FLASH, FADE_IN, FADE_OUT) wins.
/// @param fx Borrowed ScreenFX handle.
/// @return Current overlay alpha in the inclusive range 0..255.
int64_t rt_screenfx_get_overlay_alpha(rt_screenfx fx) {
    fx = checked_screenfx(fx, "ScreenFX.OverlayAlpha: expected Zanna.Game.ScreenFX");
    return fx ? fx->overlay_alpha : 0;
}

//=============================================================================
// Transition Effects
//=============================================================================

/// @brief Trigger a directional wipe transition. `direction` ∈ {LEFT, RIGHT, UP, DOWN};
/// out-of-range values default to LEFT. The colored rectangle grows from one screen edge across the
/// duration, rendered in `draw()`.
/// @param fx Borrowed ScreenFX handle.
/// @param direction Stable wipe-direction identifier.
/// @param color Packed 0x00RRGGBB Canvas color.
/// @param duration Positive effect duration in milliseconds.
void rt_screenfx_wipe(rt_screenfx fx, int64_t direction, int64_t color, int64_t duration) {
    fx = checked_screenfx(fx, "ScreenFX.Wipe: expected Zanna.Game.ScreenFX");
    if (!fx || duration <= 0)
        return;
    if (direction < 0 || direction > 3)
        direction = RT_DIR_LEFT;

    int64_t slot = find_free_slot(fx);
    if (slot < 0)
        return;

    struct screenfx_effect *e = &fx->effects[slot];
    e->type = RT_SCREENFX_WIPE;
    e->color = color;
    e->intensity = direction;
    e->duration = duration;
    e->elapsed = 0;
    e->decay = 0;
    e->extra = 0;
}

/// @brief Iris-out / "closing aperture" transition centered at (cx, cy). A colored region grows
/// inward, leaving a shrinking unobscured circle. The draw pass fills scanline
/// spans outside the Euclidean circle without requiring a per-pixel mask.
/// @param fx Borrowed ScreenFX handle.
/// @param cx Horizontal aperture center in Canvas coordinates.
/// @param cy Vertical aperture center in Canvas coordinates.
/// @param color Packed 0x00RRGGBB Canvas color.
/// @param duration Positive effect duration in milliseconds.
void rt_screenfx_circle_in(
    rt_screenfx fx, int64_t cx, int64_t cy, int64_t color, int64_t duration) {
    fx = checked_screenfx(fx, "ScreenFX.CircleIn: expected Zanna.Game.ScreenFX");
    if (!fx || duration <= 0)
        return;

    int64_t slot = find_free_slot(fx);
    if (slot < 0)
        return;

    struct screenfx_effect *e = &fx->effects[slot];
    e->type = RT_SCREENFX_CIRCLE_IN;
    e->color = color;
    e->intensity = 0;
    e->duration = duration;
    e->elapsed = 0;
    e->decay = cx;
    e->extra = cy;
}

/// @brief Iris-in / "opening aperture" transition centered at (cx, cy). The colored region recedes
/// from the screen as a growing visible circle reveals the underlying scene. The reverse companion
/// to `circle_in` — pair them across a scene boundary for a smooth iris transition.
/// @param fx Borrowed ScreenFX handle.
/// @param cx Horizontal aperture center in Canvas coordinates.
/// @param cy Vertical aperture center in Canvas coordinates.
/// @param color Packed 0x00RRGGBB Canvas color.
/// @param duration Positive effect duration in milliseconds.
void rt_screenfx_circle_out(
    rt_screenfx fx, int64_t cx, int64_t cy, int64_t color, int64_t duration) {
    fx = checked_screenfx(fx, "ScreenFX.CircleOut: expected Zanna.Game.ScreenFX");
    if (!fx || duration <= 0)
        return;

    int64_t slot = find_free_slot(fx);
    if (slot < 0)
        return;

    struct screenfx_effect *e = &fx->effects[slot];
    e->type = RT_SCREENFX_CIRCLE_OUT;
    e->color = color;
    e->intensity = 0;
    e->duration = duration;
    e->elapsed = 0;
    e->decay = cx;
    e->extra = cy;
}

/// @brief Trigger an ordered-dither dissolve transition. Uses a 4×4 Bayer matrix (Mac-style
/// dissolve): each pixel's threshold is its `bayer4x4[y%4][x%4]` value, and a global threshold
/// sweeps 0→255 over the duration. Visually a pleasing "scatter" effect; rendered pixel-by-pixel
/// in `draw()` (avoid for very high-resolution canvases).
/// @param fx Borrowed ScreenFX handle.
/// @param color Packed 0x00RRGGBB Canvas color.
/// @param duration Positive effect duration in milliseconds.
void rt_screenfx_dissolve(rt_screenfx fx, int64_t color, int64_t duration) {
    fx = checked_screenfx(fx, "ScreenFX.Dissolve: expected Zanna.Game.ScreenFX");
    if (!fx || duration <= 0)
        return;

    int64_t slot = find_free_slot(fx);
    if (slot < 0)
        return;

    struct screenfx_effect *e = &fx->effects[slot];
    e->type = RT_SCREENFX_DISSOLVE;
    e->color = color;
    e->duration = duration;
    e->elapsed = 0;
    e->intensity = 0;
    e->decay = 0;
    e->extra = 0;
}

/// @brief Trigger a "pixelate" transition. Block size grows from 1→`max_block_size` (clamped
/// minimum 2) over the duration. The runtime cannot read pixels back, so this is approximated
/// by drawing a darkening grid overlay rather than true block-averaging — the visual hint of
/// pixelation without the readback cost.
/// @param fx Borrowed ScreenFX handle.
/// @param max_block_size Final grid spacing, clamped to at least two pixels.
/// @param duration Positive effect duration in milliseconds.
void rt_screenfx_pixelate(rt_screenfx fx, int64_t max_block_size, int64_t duration) {
    fx = checked_screenfx(fx, "ScreenFX.Pixelate: expected Zanna.Game.ScreenFX");
    if (!fx || duration <= 0)
        return;
    if (max_block_size < 2)
        max_block_size = 2;

    int64_t slot = find_free_slot(fx);
    if (slot < 0)
        return;

    struct screenfx_effect *e = &fx->effects[slot];
    e->type = RT_SCREENFX_PIXELATE;
    e->color = 0;
    e->intensity = max_block_size;
    e->duration = duration;
    e->elapsed = 0;
    e->decay = 0;
    e->extra = 0;
}

/// @brief Returns 1 when no slots are active. Useful for chaining transitions ("when fade-out
/// finishes, load the next scene"). Treats a NULL handle as finished (returns 1).
/// @param fx Borrowed ScreenFX handle.
/// @return `1` when no effect slots are occupied or @p fx is null; otherwise
///         `0`.
int8_t rt_screenfx_is_finished(rt_screenfx fx) {
    fx = checked_screenfx(fx, "ScreenFX.IsFinished: expected Zanna.Game.ScreenFX");
    if (!fx)
        return 1;

    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        if (fx->effects[i].type != RT_SCREENFX_NONE)
            return 0;
    }
    return 1;
}

/// @brief Read the progress (0–1000 per mille) of the first active transition effect (WIPE,
/// CIRCLE_*, DISSOLVE, PIXELATE). Returns 0 when no transition is running, 1000 when complete.
/// Used to drive scene-load timing — kick off the next scene at progress=500 (mid-transition).
/// @param fx Borrowed ScreenFX handle.
/// @return First transition slot's clamped progress in 0..1000, or `0` when
///         none is active.
int64_t rt_screenfx_get_transition_progress(rt_screenfx fx) {
    fx = checked_screenfx(fx, "ScreenFX.TransitionProgress: expected Zanna.Game.ScreenFX");
    if (!fx)
        return 0;

    // Find the first active transition effect and return its progress
    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        struct screenfx_effect *e = &fx->effects[i];
        if (e->type >= RT_SCREENFX_WIPE && e->type <= RT_SCREENFX_PIXELATE) {
            if (e->duration <= 0)
                return 1000;
            return effect_progress_per_mille(e);
        }
    }
    return 0;
}

//=============================================================================
// Draw — Render transitions to Canvas
//=============================================================================

// Forward declarations for Canvas drawing functions used here.
// These are declared in rt_graphics.h which collections can include.
#include "rt_graphics.h"

/// @brief Fill @p color over every pixel OUTSIDE a disc of @p radius centered at
///        (@p cx, @p cy), scanline by scanline, leaving a genuinely circular
///        opening. Rows fully outside the disc are filled edge-to-edge; rows that
///        cross the disc are filled left of and right of the circular chord. This
///        replaces the earlier four-rectangle approximation, whose opening was a
///        square rather than a circle (VDOC-269).
/// @param canvas Borrowed Canvas handle.
/// @param cx Horizontal disc center.
/// @param cy Vertical disc center.
/// @param radius Non-negative disc radius.
/// @param screen_w Canvas width in pixels.
/// @param screen_h Canvas height in pixels.
/// @param color Packed 0x00RRGGBB fill color.
static void screenfx_draw_circle_mask(void *canvas,
                                      int64_t cx,
                                      int64_t cy,
                                      int64_t radius,
                                      int64_t screen_w,
                                      int64_t screen_h,
                                      int64_t color) {
    for (int64_t y = 0; y < screen_h; y++) {
        int64_t half = rt_screenfx_circle_half_chord(radius, y - cy);
        if (half <= 0) {
            // Entire row lies outside the disc → cover it edge to edge.
            rt_canvas_box(canvas, 0, y, screen_w, 1, color);
            continue;
        }
        int64_t clear_left = cx - half;   // first uncovered column
        int64_t clear_right = cx + half;  // first covered column past the opening
        if (clear_left > 0)
            rt_canvas_box(canvas, 0, y, clear_left, 1, color);
        if (clear_right < screen_w)
            rt_canvas_box(canvas, clear_right, y, screen_w - clear_right, 1, color);
    }
}

/// @brief 4×4 Bayer dithering matrix (values 0-15, scaled to 0-255 range).
static const uint8_t bayer4x4[4][4] = {
    {0, 128, 32, 160},
    {192, 64, 224, 96},
    {48, 176, 16, 144},
    {240, 112, 208, 80},
};

/// @brief Render every active transition effect onto the canvas, plus the FLASH/FADE_*-driven
/// alpha overlay accumulated by `update()`. Per-effect drawing strategies:
///   - **WIPE:** one growing rectangle from the chosen edge.
///   - **CIRCLE_IN/OUT:** a genuinely circular opening, filled scanline by scanline outside the
///     disc (Euclidean max-radius for full-corner coverage).
///   - **DISSOLVE:** per-pixel ordered Bayer dither vs a global 0→255 threshold.
///   - **PIXELATE:** semi-transparent grid overlay simulating block-pixelation without read-back.
/// Caller provides the canvas dimensions explicitly so this function never queries the canvas.
/// @param fx Borrowed ScreenFX handle.
/// @param canvas Borrowed Canvas handle receiving all drawing.
/// @param screen_w Positive Canvas width in pixels.
/// @param screen_h Positive Canvas height in pixels.
void rt_screenfx_draw(rt_screenfx fx, void *canvas, int64_t screen_w, int64_t screen_h) {
    fx = checked_screenfx(fx, "ScreenFX.Draw: expected Zanna.Game.ScreenFX");
    if (!fx || !canvas || screen_w <= 0 || screen_h <= 0)
        return;

    // Also draw overlay for existing fade/flash effects
    if (fx->overlay_alpha > 0) {
        rt_canvas_box_alpha(
            canvas, 0, 0, screen_w, screen_h, fx->overlay_color >> 8, fx->overlay_alpha);
    }

    for (int64_t i = 0; i < fx->effect_capacity; i++) {
        struct screenfx_effect *e = &fx->effects[i];
        if (e->type == RT_SCREENFX_NONE)
            continue;
        if (e->duration <= 0)
            continue;

        int64_t progress = effect_progress_per_mille(e);

        switch (e->type) {
            case RT_SCREENFX_WIPE: {
                int64_t dir = e->intensity;
                int64_t color = e->color;

                switch (dir) {
                    case RT_DIR_RIGHT: {
                        int64_t w = screen_w * progress / 1000;
                        rt_canvas_box(canvas, screen_w - w, 0, w, screen_h, color);
                        break;
                    }
                    case RT_DIR_UP: {
                        int64_t h = screen_h * progress / 1000;
                        rt_canvas_box(canvas, 0, 0, screen_w, h, color);
                        break;
                    }
                    case RT_DIR_DOWN: {
                        int64_t h = screen_h * progress / 1000;
                        rt_canvas_box(canvas, 0, screen_h - h, screen_w, h, color);
                        break;
                    }
                    default: // LEFT
                    {
                        int64_t w = screen_w * progress / 1000;
                        rt_canvas_box(canvas, 0, 0, w, screen_h, color);
                        break;
                    }
                }
                break;
            }

            case RT_SCREENFX_CIRCLE_IN:
            case RT_SCREENFX_CIRCLE_OUT: {
                // A circular opening whose radius shrinks to 0 (CircleIn) or grows
                // from 0 (CircleOut). max_r is the Euclidean distance to the farthest
                // corner so the disc fully covers/reveals the screen at its extreme.
                int64_t cx = e->decay;
                int64_t cy = e->extra;
                int64_t color = e->color;

                int64_t dx1 = cx > screen_w - cx ? cx : screen_w - cx;
                int64_t dy1 = cy > screen_h - cy ? cy : screen_h - cy;
                int64_t max_r = screenfx_isqrt(dx1 * dx1 + dy1 * dy1) + 1;

                int64_t radius = (e->type == RT_SCREENFX_CIRCLE_IN)
                                     ? max_r * (1000 - progress) / 1000
                                     : max_r * progress / 1000;

                screenfx_draw_circle_mask(canvas, cx, cy, radius, screen_w, screen_h, color);
                break;
            }

            case RT_SCREENFX_DISSOLVE: {
                // Bayer dithering: threshold increases with progress.
                // Pixels where bayer[x%4][y%4] < threshold get the overlay color.
                int64_t threshold = progress * 256 / 1000; // 0-255
                int64_t color = e->color;

                // Draw in 4×4 tile blocks for efficiency
                for (int64_t ty = 0; ty < screen_h; ty += 4) {
                    for (int64_t tx = 0; tx < screen_w; tx += 4) {
                        for (int by = 0; by < 4 && ty + by < screen_h; by++) {
                            for (int bx = 0; bx < 4 && tx + bx < screen_w; bx++) {
                                if (bayer4x4[by][bx] < (uint8_t)threshold) {
                                    rt_canvas_plot(canvas, tx + bx, ty + by, color);
                                }
                            }
                        }
                    }
                }
                break;
            }

            case RT_SCREENFX_PIXELATE: {
                // Block size grows from 1 to max_block_size over duration.
                // We render colored blocks at increasing sizes.
                // Note: true pixelation requires reading back pixels — we approximate
                // by drawing a grid of solid blocks with the average color.
                // Since we can't read pixels without a canvas read-back, we draw
                // a semi-transparent overlay grid to simulate the effect.
                int64_t max_block = e->intensity;
                int64_t block = 1 + (max_block - 1) * progress / 1000;
                if (block < 2)
                    break; // No visible effect at block=1

                // Draw grid lines to create pixelation appearance
                int64_t alpha = progress * 128 / 1000; // Gradually increasing
                if (alpha > 128)
                    alpha = 128;

                for (int64_t gy = 0; gy < screen_h; gy += block) {
                    rt_canvas_box_alpha(canvas, 0, gy, screen_w, 1, 0x000000, alpha);
                }
                for (int64_t gx = 0; gx < screen_w; gx += block) {
                    rt_canvas_box_alpha(canvas, gx, 0, 1, screen_h, 0x000000, alpha);
                }
                break;
            }

            default:
                break;
        }
    }
}
