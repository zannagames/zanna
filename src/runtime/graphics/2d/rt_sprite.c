//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_sprite.c
// Purpose: Implements reference-counted 2D sprites backed by one or more
//   retained Pixels frames. A sprite stores its position, integer-pixel
//   transform origin, percentage scale, rotation, depth, visibility, flip
//   flags, and animation timing. Drawing prepares and caches transformed
//   Pixels, then alpha-blits them to a Canvas.
//
// Key invariants:
//   - Every frame entry is a retained Pixels object. GIF loading transfers all
//     decoded frames into the sprite; other supported formats create one frame.
//   - Position (x, y) is the destination point to which the integer-pixel
//     origin is anchored. The default origin (0, 0) places the frame's top-left
//     corner at that position.
//   - Scale is an integer percentage, clamped to at least 1. Flip flags perform
//     mirroring; negative scale is not a mirroring mechanism.
//   - Transform preparation never mutates a stored frame. Any required flip,
//     scale, rotation, tint, or alpha operation works on newly allocated Pixels.
//   - The transform cache is keyed by frame pointer and transform values. An
//     in-place mutation of a frame is not observable until the key changes.
//   - Invisible sprites are skipped by drawing and hit testing, but their
//     frame timers may still advance when explicitly updated.
//
// Ownership/Lifetime:
//   - Sprite and SpriteAnimator objects use the runtime reference-counted object
//     model. The sprite finalizer releases all retained frames and its cached
//     transformed Pixels. Callers receive owned references from constructors.
//
// Links: src/runtime/graphics/2d/rt_sprite.h (public API),
//        src/runtime/graphics/2d/rt_spritesheet.h (atlas frames),
//        docs/zannalib/game.md (Sprite section)
//
//===----------------------------------------------------------------------===//

#include "rt_sprite.h"

#include "rt_file_stdio.h"
#include "rt_gif.h"
#include "rt_graphics.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_string.h"
#include "rt_time.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPRITE_INITIAL_FRAME_CAPACITY 4
#define SPRITE_DEFAULT_FRAME_DELAY_MS 100

/// @brief Sprite implementation structure.
typedef struct rt_sprite_impl {
    int64_t x;                ///< X position
    int64_t y;                ///< Y position
    int64_t scale_x;          ///< Horizontal scale (100 = 100%)
    int64_t scale_y;          ///< Vertical scale (100 = 100%)
    int64_t rotation;         ///< Rotation in degrees
    int64_t depth;            ///< Depth used by SpriteBatch sorting
    int64_t visible;          ///< Visibility flag
    int64_t origin_x;         ///< Origin X for rotation/scaling
    int64_t origin_y;         ///< Origin Y for rotation/scaling
    int64_t current_frame;    ///< Current animation frame
    int64_t frame_count;      ///< Number of frames
    int64_t frame_capacity;   ///< Allocated frame slots
    int64_t frame_delay_ms;   ///< Delay between frames
    int64_t last_frame_time;  ///< Last frame update time
    int64_t flip_x;           ///< Horizontal flip flag
    int64_t flip_y;           ///< Vertical flip flag
    void **frames;            ///< Frame pixel buffers
    int64_t *frame_delays_ms; ///< Per-frame delays
    // Transformed-frame cache. sprite_prepare_pixels reuses cache_pixels when every
    // input below is unchanged, so a rotating/scaled/tinted sprite does not re-run
    // the flip→scale→pad→rotate→tint→alpha pipeline (and its allocations) every
    // frame. The cache OWNS its buffer and hands callers a borrowed pointer (they
    // must not release it); it is evicted on the next miss and freed in the
    // finalizer. Keyed on all transform inputs plus the frame pointer, so any
    // changed setter simply misses and recomputes — no explicit invalidation.
    // Note: an in-place mutation of a frame's own pixels (same frame pointer, same
    // params) is not detected; re-set the frame or a transform param to refresh.
    void *cache_pixels;        ///< Cache-owned transformed result, or NULL.
    void *cache_frame;         ///< Frame pointer the cached result was built from.
    int64_t cache_flip_x;      ///< Cached flip_x.
    int64_t cache_flip_y;      ///< Cached flip_y.
    int64_t cache_origin_x;    ///< Cached sprite origin_x input.
    int64_t cache_origin_y;    ///< Cached sprite origin_y input.
    int64_t cache_scale_x;     ///< Cached (normalized) scale_x.
    int64_t cache_scale_y;     ///< Cached (normalized) scale_y.
    int64_t cache_rotation;    ///< Cached rotation.
    int64_t cache_tint;        ///< Cached tint_color.
    int64_t cache_alpha;       ///< Cached alpha.
    int64_t cache_out_origin_x; ///< Cached computed origin_x output.
    int64_t cache_out_origin_y; ///< Cached computed origin_y output.
    int8_t cache_out_centered;  ///< Cached origin_centered output.
    int8_t cache_valid;         ///< 1 when cache_pixels holds a usable result.
} rt_sprite_impl;

/// @brief Validate-and-cast an opaque sprite pointer to its impl, trapping on failure.
/// @details Used by every public Sprite entry point that requires a non-NULL,
///          properly-typed sprite. NULL or wrong-class IDs raise a trap with
///          @p op as the diagnostic message (or a generic fallback).
/// @param sprite_ptr Opaque sprite handle. Must be non-NULL with class id RT_SPRITE_CLASS_ID.
/// @param op         Operation name used in the trap message; may be NULL.
/// @return The downcast impl pointer (never NULL — function traps before returning NULL).
static rt_sprite_impl *sprite_checked(void *sprite_ptr, const char *op) {
    if (!sprite_ptr) {
        rt_trap(op ? op : "Sprite: null sprite");
        return NULL;
    }
    if (!rt_obj_is_instance(sprite_ptr, RT_SPRITE_CLASS_ID, sizeof(rt_sprite_impl))) {
        rt_trap(op ? op : "Sprite: invalid sprite");
        return NULL;
    }
    return (rt_sprite_impl *)sprite_ptr;
}

/// @brief Soft variant of sprite_checked — returns NULL instead of trapping.
/// @details Used by lifetime-sensitive paths like the GC finalizer and
///          Draw() loops where a stale handle is not a programming error
///          and should be silently skipped.
/// @param sprite_ptr Opaque candidate sprite handle; may be `NULL`.
/// @return The validated implementation pointer, or `NULL` for a null, stale,
///         undersized, or wrong-class object.
static rt_sprite_impl *sprite_checked_or_null(void *sprite_ptr) {
    if (!sprite_ptr || !rt_obj_is_instance(sprite_ptr, RT_SPRITE_CLASS_ID, sizeof(rt_sprite_impl)))
        return NULL;
    return (rt_sprite_impl *)sprite_ptr;
}

/// @brief Return the active frame's Pixels object, or NULL if none / out-of-range.
/// @param sprite Sprite whose active frame is requested; may be `NULL`.
/// @return A borrowed Pixels pointer for `current_frame`, or `NULL` when the
///         sprite has no valid active frame.
static void *sprite_get_current_frame_ptr(rt_sprite_impl *sprite) {
    if (!sprite || sprite->frame_count <= 0 || sprite->current_frame < 0 ||
        sprite->current_frame >= sprite->frame_count || !sprite->frames)
        return NULL;
    return sprite->frames[sprite->current_frame];
}

/// @brief Clamp a per-frame delay to a minimum of 1 ms (a 0/negative delay
///        would make the animation never advance / divide by zero).
/// @param ms Requested delay in milliseconds.
/// @return @p ms when positive, otherwise `1`.
static int64_t sprite_normalize_delay(int64_t ms) {
    return ms < 1 ? 1 : ms;
}

/// @brief Grow the parallel frames[] / frame_delays_ms[] arrays atomically.
/// @details Allocates replacement arrays first, copies existing frame pointers
///          and delay values into them, initializes new slots to NULL/default
///          delay, then swaps both pointers together. This avoids the partial
///          `realloc` state where one array has moved and the other allocation
///          fails, which would corrupt the sprite's frame bookkeeping.
/// @param sprite Sprite whose frame storage should grow.
/// @param needed Minimum number of frame slots required.
/// @return 1 on success, 0 on invalid input, overflow, or allocation failure.
static int8_t sprite_ensure_frame_capacity(rt_sprite_impl *sprite, int64_t needed) {
    if (!sprite || needed <= 0)
        return 0;
    if (needed <= sprite->frame_capacity)
        return 1;
    int64_t new_cap =
        sprite->frame_capacity > 0 ? sprite->frame_capacity : SPRITE_INITIAL_FRAME_CAPACITY;
    while (new_cap < needed) {
        if (new_cap > INT64_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    if ((uint64_t)new_cap > (uint64_t)SIZE_MAX / sizeof(void *) ||
        (uint64_t)new_cap > (uint64_t)SIZE_MAX / sizeof(int64_t))
        return 0;

    void **new_frames = (void **)malloc((size_t)new_cap * sizeof(void *));
    int64_t *new_delays = (int64_t *)malloc((size_t)new_cap * sizeof(int64_t));
    if (!new_frames || !new_delays) {
        free(new_frames);
        free(new_delays);
        return 0;
    }

    for (int64_t i = 0; i < sprite->frame_capacity; i++) {
        new_frames[i] = sprite->frames ? sprite->frames[i] : NULL;
        new_delays[i] =
            sprite->frame_delays_ms ? sprite->frame_delays_ms[i] : sprite->frame_delay_ms;
    }

    for (int64_t i = sprite->frame_capacity; i < new_cap; i++) {
        new_frames[i] = NULL;
        new_delays[i] =
            sprite->frame_delay_ms > 0 ? sprite->frame_delay_ms : SPRITE_DEFAULT_FRAME_DELAY_MS;
    }
    free(sprite->frames);
    free(sprite->frame_delays_ms);
    sprite->frames = new_frames;
    sprite->frame_delays_ms = new_delays;
    sprite->frame_capacity = new_cap;
    return 1;
}

/// @brief Normalized display delay (ms) for a given @p frame, falling back to
///        the sprite-wide default for out-of-range frames or missing per-frame
///        delay data.
/// @param sprite Sprite containing the delay table; may be `NULL`.
/// @param frame Frame index to inspect.
/// @return A positive delay in milliseconds. A null sprite yields the compile-time
///         default; invalid indices or absent storage use the sprite-wide delay.
static int64_t sprite_frame_delay_at(rt_sprite_impl *sprite, int64_t frame) {
    if (!sprite || frame < 0 || frame >= sprite->frame_count || !sprite->frame_delays_ms)
        return sprite ? sprite_normalize_delay(sprite->frame_delay_ms)
                      : SPRITE_DEFAULT_FRAME_DELAY_MS;
    return sprite_normalize_delay(sprite->frame_delays_ms[frame]);
}

/// @brief Total duration (ms) of one full animation cycle — the sum of every
///        frame's normalized delay (0 if the sprite has no frames).
/// @param sprite Sprite whose frame delays should be summed; may be `NULL`.
/// @return The saturated cycle duration in milliseconds, or `0` for no frames.
static int64_t sprite_cycle_delay(rt_sprite_impl *sprite) {
    if (!sprite || sprite->frame_count <= 0)
        return 0;
    int64_t total = 0;
    for (int64_t i = 0; i < sprite->frame_count; i++) {
        int64_t delay = sprite_frame_delay_at(sprite, i);
        if (total > INT64_MAX - delay)
            return INT64_MAX;
        total += delay;
    }
    return total;
}

/// @brief Multiply every pixel's alpha channel by @p alpha divided by 255.
/// @details The multiplication is rounded to the nearest integer. Values below
///          zero are clamped to transparent, values at or above 255 are a no-op,
///          and invalid Pixels objects are ignored.
/// @param pixels Mutable Pixels object whose RGBA words should be updated.
/// @param alpha Global alpha multiplier in the nominal range 0 through 255.
static void sprite_apply_alpha(void *pixels, int64_t alpha) {
    if (!pixels || alpha >= 255)
        return;
    if (alpha < 0)
        alpha = 0;
    rt_pixels_impl *impl = rt_pixels_checked_impl_or_null(pixels);
    if (!impl)
        return;
    if (!impl->data)
        return;
    uint32_t *data = impl->data;
    int64_t count = impl->width * impl->height;
    for (int64_t i = 0; i < count; i++) {
        uint32_t rgba = data[i];
        uint32_t a = rgba & 0xFFu;
        a = (a * (uint32_t)alpha + 127u) / 255u;
        data[i] = (rgba & 0xFFFFFF00u) | a;
    }
}

/// @brief Clamp a sprite scale percentage to a minimum of 1 (never zero or negative).
/// @details A scale of 0 would divide by zero in `sprite_saturating_scale`; negative
///   values are semantically invalid (use flip flags for mirroring). Clamping to 1
///   produces a 1% scale rather than asserting so the sprite renders as a near-invisible
///   single pixel rather than crashing.
/// @param scale Requested percentage scale.
/// @return @p scale when positive, otherwise `1`.
static int64_t sprite_normalize_scale(int64_t scale) {
    return scale < 1 ? 1 : scale;
}

/// @brief Multiply @p value by a percentage @p scale (e.g., 200 = double) with saturation.
/// @details The computation `value * scale / 100` is done in `long double` to avoid
///   integer overflow before the division. The result is rounded to the nearest
///   integer (away from zero) rather than truncating, which keeps scaled coordinates
///   from drifting when repeatedly scaled and unscaled. Saturates to INT64_MAX/MIN
///   rather than wrapping on overflow.
/// @param value Signed coordinate or dimension to scale.
/// @param scale Percentage scale; values below 1 are normalized to 1.
/// @return The nearest scaled integer, saturated to the `int64_t` range.
static int64_t sprite_saturating_scale(int64_t value, int64_t scale) {
    long double scaled = ((long double)value * (long double)sprite_normalize_scale(scale)) / 100.0L;
    if (scaled >= (long double)INT64_MAX)
        return INT64_MAX;
    if (scaled <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)(scaled >= 0.0L ? scaled + 0.5L : scaled - 0.5L);
}

/// @brief Add two int64_t values with saturation at INT64_MAX / INT64_MIN.
/// @details Standard overflow-safe addition: checks whether the result would exceed
///   the representable range before performing the addition. Used to safely compute
///   the last coordinate of a sprite region (origin + length - 1) without wrapping
///   on pathological dimension values supplied from untrusted asset data.
/// @param a Left addend.
/// @param b Right addend.
/// @return The mathematical sum when representable, otherwise the corresponding
///         `int64_t` limit.
static int64_t sprite_add_saturating(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Subtract two int64_t values with saturation, computed via long double.
/// @details Uses long double to avoid integer overflow during the intermediate
///   computation (INT64_MIN - 1 would wrap in signed integer arithmetic). Saturates
///   rather than wrapping to keep pixel coordinate arithmetic predictable when
///   dealing with extreme or adversarial dimension values.
/// @param a Minuend.
/// @param b Subtrahend.
/// @return The mathematical difference when representable, otherwise the
///         corresponding `int64_t` limit.
static int64_t sprite_sub_saturating(int64_t a, int64_t b) {
    long double value = (long double)a - (long double)b;
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)value;
}

/// @brief Test whether two 1D intervals [a0, a0+a_len) and [b0, b0+b_len) overlap.
/// @details Used for sprite-region intersection checks (e.g., clip-rect vs. draw-rect).
///   Both lengths must be positive for an overlap to be possible; a zero-length interval
///   is treated as empty. The endpoint computation uses `sprite_add_saturating` to
///   prevent overflow when length is near INT64_MAX.
/// @param a0 Inclusive start of the first interval.
/// @param a_len Length of the first interval.
/// @param b0 Inclusive start of the second interval.
/// @param b_len Length of the second interval.
/// @return 1 if the intervals share at least one point, 0 otherwise.
static int8_t sprite_interval_overlaps(int64_t a0, int64_t a_len, int64_t b0, int64_t b_len) {
    if (a_len <= 0 || b_len <= 0)
        return 0;
    int64_t a_last = sprite_add_saturating(a0, a_len - 1);
    int64_t b_last = sprite_add_saturating(b0, b_len - 1);
    return a0 <= b_last && b0 <= a_last;
}

/// @brief Test whether @p point falls within the closed interval [start, start+len-1].
/// @details Complement to `sprite_interval_overlaps` for point-in-region tests.
///   Returns 0 immediately for empty intervals (len ≤ 0) or points before start.
///   Overflow-safe endpoint via `sprite_add_saturating`.
/// @param start Inclusive start of the interval.
/// @param len Length of the interval.
/// @param point Coordinate to test.
/// @return 1 if start <= point <= start+len-1, 0 otherwise.
static int8_t sprite_interval_contains(int64_t start, int64_t len, int64_t point) {
    if (len <= 0 || point < start)
        return 0;
    int64_t last = sprite_add_saturating(start, len - 1);
    return point <= last;
}

/// @brief Scale a pixel origin by the sprite scale percentage (1..100..n).
/// @param origin Unscaled local-space origin coordinate.
/// @param scale Percentage scale; values below 1 are normalized to 1.
/// @return The scaled coordinate, rounded and saturated by
///         `sprite_saturating_scale()`.
static int64_t sprite_scale_origin(int64_t origin, int64_t scale) {
    return sprite_saturating_scale(origin, scale);
}

/// @brief Saturating absolute value: handles the INT64_MIN edge case that overflows
///        with plain negation (`-INT64_MIN == INT64_MIN` due to two's complement).
/// @param value Signed value to make nonnegative.
/// @return |value|, saturated to INT64_MAX when value == INT64_MIN.
static int64_t sprite_abs_sat(int64_t value) {
    if (value == INT64_MIN)
        return INT64_MAX;
    return value < 0 ? -value : value;
}

/// @brief Subtract @p b from @p a with saturation — distinct from `sprite_sub_saturating`
///        in using integer range checks rather than long double arithmetic.
/// @details Handles the `a - b` overflow cases: if b is negative and a is near
///   INT64_MAX the result wraps upward; if b is positive and a is near INT64_MIN the
///   result wraps downward. Both are caught by the inequality `a < INT64_MIN + b`
///   (respectively `a > INT64_MAX + b`).
/// @param a Minuend.
/// @param b Subtrahend.
/// @return The mathematical difference when representable, otherwise the
///         corresponding `int64_t` limit.
static int64_t sprite_saturating_sub(int64_t a, int64_t b) {
    if (b < 0 && a > INT64_MAX + b)
        return INT64_MAX;
    if (b > 0 && a < INT64_MIN + b)
        return INT64_MIN;
    return a - b;
}

/// @brief Normalize a tint color value to either the "no tint" sentinel or a color value.
/// @details -1 is the "no tint" sentinel. Non-negative values are passed through so
///   Color.RGBA tagged alpha and raw RGBA tint alpha survive into rt_pixels_tint().
/// @param tint_color Runtime color value, or any negative value for no tint.
/// @return `-1` for a negative input; otherwise @p tint_color unchanged.
static int64_t sprite_normalize_tint(int64_t tint_color) {
    if (tint_color < 0)
        return -1;
    return tint_color;
}

/// @brief Release a transformed-pixels buffer if it isn't the original frame.
///
/// `sprite_prepare_pixels` may either return the frame unchanged
/// (no transform applied) or a freshly-allocated working copy.
/// This helper drops the temporary without ever releasing the
/// canonical frame stored in `sprite->frames`.
/// @param pixels Candidate transformed Pixels object; may be `NULL`.
/// @param frame Borrowed canonical frame that must not be released.
static void sprite_release_if_owned(void *pixels, void *frame) {
    if (pixels && pixels != frame)
        rt_heap_release(pixels);
}

/// @brief Release a GC-managed object unconditionally — utility wrapper for frame teardown.
/// @details Used during GIF frame cleanup where the caller has already confirmed the
///   pixels pointer is non-NULL. If the object's refcount drops to zero, `rt_obj_free`
///   is called to hand the memory back to the pool.
/// @param obj Runtime-managed object whose reference should be released; `NULL`
///            is accepted as a no-op.
static void sprite_release_object(void *obj) {
    if (!obj)
        return;
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Release all pixel objects in a GIF frame array and then free the array itself.
/// @details Each frame's `pixels` is a GC-managed Pixels object; the frame metadata
///   struct is heap-allocated outside the GC. The GC objects are released first (so
///   pool memory is returned), then the containing array is freed with `free()`.
/// @param frames Heap-allocated array of gif_frame_t structs; may be NULL (no-op).
/// @param count  Number of valid entries in @p frames.
static void sprite_release_gif_frames(gif_frame_t *frames, int count) {
    if (!frames)
        return;
    for (int i = 0; i < count; ++i)
        sprite_release_object(frames[i].pixels);
    free(frames);
}

/// @brief Lazily clone a frame the first time we need to mutate it.
///
/// If `transformed` already points to a temporary copy we return
/// it as-is; if it still aliases the original `frame` we make a
/// fresh clone. Avoids ever destructively editing the frame stored
/// inside the sprite.
/// @param transformed Current borrowed-frame or owned-temporary pointer.
/// @param frame Borrowed canonical frame used to detect aliasing.
/// @return @p transformed when already independently mutable, a new Pixels clone
///         when it aliases @p frame, or `NULL` on invalid input/allocation failure.
static void *sprite_clone_for_edit(void *transformed, void *frame) {
    if (!transformed || transformed != frame)
        return transformed;
    return rt_pixels_clone(frame);
}

/// @brief Swap an intermediate transform result into `*slot`, freeing the prior temp.
///
/// Used by the transform pipeline (flip → scale → rotate → tint) to
/// chain operations: each step replaces the working buffer and the
/// previous temp gets released — but never the original frame.
/// @param replacement Newly produced Pixels object, or `NULL` on transform failure.
/// @param slot Address of the current working Pixels pointer.
/// @param frame Borrowed canonical frame that must never be released here.
/// @return The new value of `*slot`, or `NULL` after cleaning up a failed step.
static void *sprite_replace_pixels(void *replacement, void **slot, void *frame) {
    if (!replacement) {
        if (slot && *slot && *slot != frame)
            rt_heap_release(*slot);
        if (slot)
            *slot = NULL;
        return NULL;
    }
    if (replacement == *slot)
        return *slot;
    if (*slot && *slot != frame)
        rt_heap_release(*slot);
    *slot = replacement;
    return *slot;
}

/// @brief Apply the sprite's flip / scale / rotation / tint / alpha to its current frame.
///
/// Returns a Pixels object suitable for blitting: either the unmodified frame
/// (when no transforms apply) or a transformed buffer owned by the sprite's
/// transform cache. In BOTH cases the returned pointer is BORROWED — the caller
/// must NOT release it. The cache buffer is evicted on the next call whose
/// transform key differs, and freed in the sprite finalizer. On internal failure
/// the partial buffer is released here and NULL is returned.
/// Also fills in the post-transform origin and a flag indicating whether the
/// rotation step expanded the canvas to a centered square.
///
/// The pipeline order is horizontal/vertical flip, nearest-neighbor scale,
/// optional transparent padding around a non-centered rotation origin, rotation,
/// tint, and explicit alpha. The cache key does not include a Pixels mutation
/// generation, so callers that edit a frame in place must change a key input to
/// force regeneration.
/// @param sprite Valid sprite whose current frame and flip/origin state are used.
/// @param scale_x Horizontal percentage scale; values below 1 become 1.
/// @param scale_y Vertical percentage scale; values below 1 become 1.
/// @param rotation Clockwise rotation in degrees, without normalization.
/// @param tint_color Tint accepted by `rt_pixels_tint()`, or negative for none.
/// @param alpha Global alpha multiplier; values below zero become transparent
///              and values at or above 255 leave frame alpha unchanged.
/// @param origin_x_out Optional destination for the scaled or padded X origin.
/// @param origin_y_out Optional destination for the scaled or padded Y origin.
/// @param origin_centered_out Optional destination set to 1 when rotation makes
///                            the transformed image center the draw origin.
/// @return A borrowed current frame or cache-owned transformed Pixels object;
///         `NULL` when no frame exists or transformation fails.
static void *sprite_prepare_pixels(rt_sprite_impl *sprite,
                                   int64_t scale_x,
                                   int64_t scale_y,
                                   int64_t rotation,
                                   int64_t tint_color,
                                   int64_t alpha,
                                   int64_t *origin_x_out,
                                   int64_t *origin_y_out,
                                   int8_t *origin_centered_out) {
    void *frame = sprite_get_current_frame_ptr(sprite);
    if (!frame)
        return NULL;

    scale_x = sprite_normalize_scale(scale_x);
    scale_y = sprite_normalize_scale(scale_y);

    /* Cache lookup: if every transform input matches the last prepared result,
     * return the cache-owned buffer directly (the caller borrows it and must not
     * release it — see rt_sprite_draw_transformed). */
    int64_t tint_key = sprite_normalize_tint(tint_color);
    if (sprite->cache_valid && sprite->cache_pixels && sprite->cache_frame == frame &&
        sprite->cache_flip_x == sprite->flip_x && sprite->cache_flip_y == sprite->flip_y &&
        sprite->cache_origin_x == sprite->origin_x &&
        sprite->cache_origin_y == sprite->origin_y && sprite->cache_scale_x == scale_x &&
        sprite->cache_scale_y == scale_y && sprite->cache_rotation == rotation &&
        sprite->cache_tint == tint_key && sprite->cache_alpha == alpha) {
        if (origin_x_out)
            *origin_x_out = sprite->cache_out_origin_x;
        if (origin_y_out)
            *origin_y_out = sprite->cache_out_origin_y;
        if (origin_centered_out)
            *origin_centered_out = sprite->cache_out_centered;
        return sprite->cache_pixels;
    }

    void *transformed = frame;

    if (sprite->flip_x) {
        void *flipped = rt_pixels_flip_h(transformed);
        transformed = sprite_replace_pixels(flipped, &transformed, frame);
        if (!transformed)
            return NULL;
    }
    if (sprite->flip_y) {
        void *flipped = rt_pixels_flip_v(transformed);
        transformed = sprite_replace_pixels(flipped, &transformed, frame);
        if (!transformed)
            return NULL;
    }

    if (!transformed)
        return NULL;

    if (scale_x != 100 || scale_y != 100) {
        int64_t new_w = sprite_saturating_scale(rt_pixels_width(transformed), scale_x);
        int64_t new_h = sprite_saturating_scale(rt_pixels_height(transformed), scale_y);
        if (new_w < 1)
            new_w = 1;
        if (new_h < 1)
            new_h = 1;
        void *scaled = rt_pixels_scale(transformed, new_w, new_h);
        transformed = sprite_replace_pixels(scaled, &transformed, frame);
        if (!transformed)
            return NULL;
    }

    /* Flip semantics: rt_pixels_flip_h/v mirror the frame around its center and the
     * origin is a fixed rect-space reference (not an artwork-feature anchor), so a
     * flipped sprite mirrors in place within the same screen rectangle — the
     * standard flip_h/flip_v convention. The origin is therefore NOT remapped on
     * flip; center the origin (width/2, height/2) if you need a pivot that stays on
     * the same artwork feature across a flip. */
    int64_t origin_x = sprite_scale_origin(sprite->origin_x, scale_x);
    int64_t origin_y = sprite_scale_origin(sprite->origin_y, scale_y);
    int8_t origin_centered = 0;

    if (rotation != 0) {
        int64_t src_w = rt_pixels_width(transformed);
        int64_t src_h = rt_pixels_height(transformed);
        int64_t center_x = src_w / 2;
        int64_t center_y = src_h / 2;

        if (origin_x != center_x || origin_y != center_y) {
            int64_t half_w = sprite_abs_sat(origin_x);
            int64_t edge_w = sprite_abs_sat(sprite_saturating_sub(src_w, origin_x));
            int64_t half_h = sprite_abs_sat(origin_y);
            int64_t edge_h = sprite_abs_sat(sprite_saturating_sub(src_h, origin_y));
            if (edge_w > half_w)
                half_w = edge_w;
            if (edge_h > half_h)
                half_h = edge_h;
            if (half_w < 1)
                half_w = 1;
            if (half_h < 1)
                half_h = 1;

            if (half_w > INT64_MAX / 2 || half_h > INT64_MAX / 2) {
                sprite_release_if_owned(transformed, frame);
                rt_trap("Sprite.DrawTransformed: transformed dimensions too large");
                return NULL;
            }
            void *padded = rt_pixels_new(half_w * 2, half_h * 2);
            if (!padded) {
                sprite_release_if_owned(transformed, frame);
                return NULL;
            }
            rt_pixels_copy(
                padded, half_w - origin_x, half_h - origin_y, transformed, 0, 0, src_w, src_h);
            transformed = sprite_replace_pixels(padded, &transformed, frame);
            if (!transformed)
                return NULL;
            origin_x = half_w;
            origin_y = half_h;
        }

        void *rotated = rt_pixels_rotate(transformed, (double)rotation);
        transformed = sprite_replace_pixels(rotated, &transformed, frame);
        if (!transformed)
            return NULL;
        origin_centered = 1;
    }

    int64_t tint = sprite_normalize_tint(tint_color);
    if (tint >= 0) {
        void *tinted = rt_pixels_tint(transformed, tint);
        transformed = sprite_replace_pixels(tinted, &transformed, frame);
        if (!transformed)
            return NULL;
    }

    if (alpha < 255) {
        transformed = sprite_clone_for_edit(transformed, frame);
        if (!transformed)
            return NULL;
        sprite_apply_alpha(transformed, alpha);
    }

    if (origin_x_out)
        *origin_x_out = origin_x;
    if (origin_y_out)
        *origin_y_out = origin_y;
    if (origin_centered_out)
        *origin_centered_out = origin_centered;

    /* Store the freshly computed result in the cache (only an owned buffer, never
     * the frame itself). The cache takes over the single reference this function's
     * pipeline produced; evict the previous cached buffer first. The caller borrows
     * the returned pointer and does not release it. */
    if (transformed && transformed != frame) {
        if (sprite->cache_pixels && sprite->cache_pixels != transformed)
            rt_heap_release(sprite->cache_pixels);
        sprite->cache_pixels = transformed;
        sprite->cache_frame = frame;
        sprite->cache_flip_x = sprite->flip_x;
        sprite->cache_flip_y = sprite->flip_y;
        sprite->cache_origin_x = sprite->origin_x;
        sprite->cache_origin_y = sprite->origin_y;
        sprite->cache_scale_x = scale_x;
        sprite->cache_scale_y = scale_y;
        sprite->cache_rotation = rotation;
        sprite->cache_tint = tint_key;
        sprite->cache_alpha = alpha;
        sprite->cache_out_origin_x = origin_x;
        sprite->cache_out_origin_y = origin_y;
        sprite->cache_out_centered = origin_centered;
        sprite->cache_valid = 1;
    }
    return transformed;
}

/// @brief Release all storage owned by a sprite during runtime finalization.
/// @details Each retained frame and the cache-owned transform result lose one
///          reference. The parallel frame/delay arrays are freed and reset so
///          accidental repeated finalization is harmless.
/// @param obj Candidate sprite object supplied by the runtime finalizer.
static void sprite_finalize(void *obj) {
    rt_sprite_impl *sprite = sprite_checked_or_null(obj);
    if (!sprite)
        return;
    for (int64_t i = 0; i < sprite->frame_count; i++) {
        if (sprite->frames && sprite->frames[i])
            rt_heap_release(sprite->frames[i]);
    }
    if (sprite->cache_pixels) {
        rt_heap_release(sprite->cache_pixels);
        sprite->cache_pixels = NULL;
        sprite->cache_valid = 0;
    }
    free(sprite->frames);
    free(sprite->frame_delays_ms);
    sprite->frames = NULL;
    sprite->frame_delays_ms = NULL;
    sprite->frame_capacity = 0;
    sprite->frame_count = 0;
}

/// @brief Allocate and initialize an empty runtime-managed sprite.
/// @details Establishes the default position `(0,0)`, 100% scales, zero
///          rotation/depth/origin, visible state, frame zero, 100 ms default
///          delay, no flips, and an empty transform cache. The sprite finalizer
///          is registered before the object is returned.
/// @return A caller-owned sprite reference, or `NULL` if object allocation fails.
static rt_sprite_impl *sprite_alloc(void) {
    rt_sprite_impl *sprite =
        (rt_sprite_impl *)rt_obj_new_i64(RT_SPRITE_CLASS_ID, (int64_t)sizeof(rt_sprite_impl));
    if (!sprite)
        return NULL;

    sprite->x = 0;
    sprite->y = 0;
    sprite->scale_x = 100;
    sprite->scale_y = 100;
    sprite->rotation = 0;
    sprite->depth = 0;
    sprite->visible = 1;
    sprite->origin_x = 0;
    sprite->origin_y = 0;
    sprite->current_frame = 0;
    sprite->frame_count = 0;
    sprite->frame_capacity = 0;
    sprite->frame_delay_ms = SPRITE_DEFAULT_FRAME_DELAY_MS;
    sprite->last_frame_time = 0;
    sprite->flip_x = 0;
    sprite->flip_y = 0;
    sprite->frames = NULL;
    sprite->frame_delays_ms = NULL;
    sprite->cache_pixels = NULL;
    sprite->cache_valid = 0;

    rt_obj_set_finalizer(sprite, sprite_finalize);
    return sprite;
}

//=============================================================================
// Sprite Creation
//=============================================================================

/// @brief Create a sprite from an existing Pixels object as the first frame.
///
/// Bumps the Pixels refcount and stows it as `frames[0]`. Position
/// defaults to (0, 0); scale 100%; full opacity; visible.
/// @param pixels Valid runtime Pixels object. The sprite retains one reference.
/// @return A caller-owned sprite reference, or `NULL` after trapping on an
///         invalid Pixels object or when allocation fails.
/// @throws A runtime trap when @p pixels is null or not a Pixels instance.
void *rt_sprite_new(void *pixels) {
    if (!pixels) {
        rt_trap("Sprite.New: null pixels");
        return NULL;
    }
    if (!rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl))) {
        rt_trap("Sprite.New: invalid pixels");
        return NULL;
    }

    rt_sprite_impl *sprite = sprite_alloc();
    if (!sprite)
        return NULL;
    if (!sprite_ensure_frame_capacity(sprite, 1)) {
        if (rt_obj_release_check0(sprite))
            rt_obj_free(sprite);
        return NULL;
    }

    rt_obj_retain_maybe(pixels);
    sprite->frames[0] = pixels;
    sprite->frame_delays_ms[0] = sprite->frame_delay_ms;
    sprite->frame_count = 1;

    return sprite;
}

/// @brief Detect image format from file magic bytes.
/// @param filepath UTF-8 path to open and inspect.
/// @return `1` for BMP, `2` for PNG, `3` for JPEG, `4` for GIF, or `0`
///         when the file cannot be opened or its signature is unrecognized.
static int detect_image_format(const char *filepath) {
    FILE *f = rt_file_stdio_open_utf8(filepath, "rb");
    if (!f)
        return 0;
    uint8_t hdr[8];
    size_t n = fread(hdr, 1, 8, f);
    fclose(f);
    if (n >= 2 && hdr[0] == 'B' && hdr[1] == 'M')
        return 1; // BMP
    if (n >= 8 && hdr[0] == 137 && hdr[1] == 80 && hdr[2] == 78 && hdr[3] == 71)
        return 2; // PNG
    if (n >= 2 && hdr[0] == 0xFF && hdr[1] == 0xD8)
        return 3; // JPEG
    if (n >= 3 && hdr[0] == 'G' && hdr[1] == 'I' && hdr[2] == 'F')
        return 4; // GIF
    return 0;
}

/// @brief Load a signature-recognized image file into a new sprite.
/// @details BMP, PNG, and JPEG files produce a one-frame sprite. GIF decoding
///          transfers every decoded frame and its delay into the sprite, using
///          the sprite default delay for nonpositive GIF delays. Detection uses
///          file magic rather than the path extension.
/// @param path Runtime string containing the UTF-8 image path; borrowed.
/// @return A caller-owned sprite reference, or `NULL` for a null/invalid path,
///         unknown signature, decode error, invalid GIF frame, or allocation
///         failure.
void *rt_sprite_from_file(void *path) {
    if (!path)
        return NULL;

    const char *filepath = rt_string_cstr((rt_string)path);
    if (!filepath)
        return NULL;

    int fmt = detect_image_format(filepath);

    // Animated GIF: decode all frames directly into the sprite
    if (fmt == 4) {
        gif_frame_t *gif_frames = NULL;
        int gif_count = 0, gif_w = 0, gif_h = 0;
        if (gif_decode_file(filepath, &gif_frames, &gif_count, &gif_w, &gif_h) <= 0)
            return NULL;
        if (!gif_frames || gif_count <= 0) {
            sprite_release_gif_frames(gif_frames, gif_count);
            return NULL;
        }

        rt_sprite_impl *sprite = sprite_alloc();
        if (!sprite) {
            sprite_release_gif_frames(gif_frames, gif_count);
            return NULL;
        }
        int n = gif_count;
        if (!sprite_ensure_frame_capacity(sprite, n)) {
            sprite_release_gif_frames(gif_frames, gif_count);
            if (rt_obj_release_check0(sprite))
                rt_obj_free(sprite);
            return NULL;
        }
        for (int i = 0; i < n; i++) {
            if (!gif_frames[i].pixels) {
                sprite_release_gif_frames(gif_frames, gif_count);
                if (rt_obj_release_check0(sprite))
                    rt_obj_free(sprite);
                return NULL;
            }
        }
        for (int i = 0; i < n; i++) {
            sprite->frames[i] = gif_frames[i].pixels;
            sprite->frame_delays_ms[i] =
                gif_frames[i].delay_ms > 0 ? gif_frames[i].delay_ms : sprite->frame_delay_ms;
            gif_frames[i].pixels = NULL; // Transfer ownership from the decoder result array.
        }
        sprite->frame_count = n;
        if (gif_count > 0 && gif_frames[0].delay_ms > 0)
            sprite->frame_delay_ms = gif_frames[0].delay_ms;
        sprite_release_gif_frames(gif_frames, gif_count);
        return sprite;
    }

    // Single-image formats: BMP, PNG, JPEG
    void *pixels = NULL;
    switch (fmt) {
        case 1:
            pixels = rt_pixels_load_bmp(path);
            break;
        case 2:
            pixels = rt_pixels_load_png(path);
            break;
        case 3:
            pixels = rt_pixels_load_jpeg(path);
            break;
        default:
            return NULL;
    }
    if (!pixels)
        return NULL;

    rt_sprite_impl *sprite = sprite_alloc();
    if (!sprite) {
        sprite_release_object(pixels);
        return NULL;
    }
    if (!sprite_ensure_frame_capacity(sprite, 1)) {
        sprite_release_object(pixels);
        if (rt_obj_release_check0(sprite))
            rt_obj_free(sprite);
        return NULL;
    }

    sprite->frames[0] = pixels;
    sprite->frame_delays_ms[0] = sprite->frame_delay_ms;
    sprite->frame_count = 1;

    return sprite;
}

//=============================================================================
// Sprite Properties
//=============================================================================

// ---------------------------------------------------------------------------
// Property accessors — straight getters and setters for the visible
// state of a sprite. All take a `void *sprite_ptr` (the GC-managed
// `rt_sprite_impl`) and return / accept an `int64_t` so they can
// match the runtime calling convention. These accessors validate the
// handle strictly: a NULL or wrong-class handle traps (programmer
// error), unlike the draw/update/overlap paths which soft-skip a stale
// handle. The `return 0` after each check is therefore unreachable.
// `scale_x`, `scale_y`, `rotation` are stored as integers (percent
// or degrees); `tint_color` accepts 0xAARRGGBB / 0x00RRGGBB and uses
// -1 as the no-tint sentinel.
// ---------------------------------------------------------------------------

/// @brief Get the sprite position's X coordinate.
/// @param sprite_ptr Valid sprite handle.
/// @return The stored X coordinate.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_x(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.X: invalid sprite");
    if (!sprite)
        return 0;
    return sprite->x;
}

/// @brief Set the sprite position's X coordinate.
/// @param sprite_ptr Valid sprite handle.
/// @param x New destination-anchor X coordinate.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_x(void *sprite_ptr, int64_t x) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.X: invalid sprite");
    if (!sprite)
        return;
    sprite->x = x;
}

/// @brief Get the sprite position's Y coordinate.
/// @param sprite_ptr Valid sprite handle.
/// @return The stored Y coordinate.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_y(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Y: invalid sprite");
    if (!sprite)
        return 0;
    return sprite->y;
}

/// @brief Set the sprite position's Y coordinate.
/// @param sprite_ptr Valid sprite handle.
/// @param y New destination-anchor Y coordinate.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_y(void *sprite_ptr, int64_t y) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Y: invalid sprite");
    if (!sprite)
        return;
    sprite->y = y;
}

/// @brief Get the unscaled width of the current frame.
/// @param sprite_ptr Valid sprite handle.
/// @return Current-frame width in pixels, or `0` when the sprite has no frame.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_width(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Width: invalid sprite");
    if (!sprite)
        return 0;
    void *frame = sprite_get_current_frame_ptr(sprite);
    if (!frame)
        return 0;
    return rt_pixels_width(frame);
}

/// @brief Get the unscaled height of the current frame.
/// @param sprite_ptr Valid sprite handle.
/// @return Current-frame height in pixels, or `0` when the sprite has no frame.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_height(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Height: invalid sprite");
    if (!sprite)
        return 0;
    void *frame = sprite_get_current_frame_ptr(sprite);
    if (!frame)
        return 0;
    return rt_pixels_height(frame);
}

/// @brief Get the horizontal percentage scale.
/// @param sprite_ptr Valid sprite handle.
/// @return Stored horizontal scale, where `100` is unscaled.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_scale_x(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.ScaleX: invalid sprite");
    if (!sprite)
        return 100;
    return sprite->scale_x;
}

/// @brief Set horizontal scale percent (100 = unscaled, 200 = 2x wider).
/// @param sprite_ptr Valid sprite handle.
/// @param scale Requested percentage, clamped to a minimum of `1`.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_scale_x(void *sprite_ptr, int64_t scale) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.ScaleX: invalid sprite");
    if (!sprite)
        return;
    sprite->scale_x = sprite_normalize_scale(scale);
}

/// @brief Get the vertical percentage scale.
/// @param sprite_ptr Valid sprite handle.
/// @return Stored vertical scale, where `100` is unscaled.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_scale_y(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.ScaleY: invalid sprite");
    if (!sprite)
        return 100;
    return sprite->scale_y;
}

/// @brief Set vertical scale percent.
/// @param sprite_ptr Valid sprite handle.
/// @param scale Requested percentage, clamped to a minimum of `1`.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_scale_y(void *sprite_ptr, int64_t scale) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.ScaleY: invalid sprite");
    if (!sprite)
        return;
    sprite->scale_y = sprite_normalize_scale(scale);
}

/// @brief Get the stored clockwise rotation in degrees.
/// @param sprite_ptr Valid sprite handle.
/// @return The unnormalized signed degree value.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_rotation(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Rotation: invalid sprite");
    if (!sprite)
        return 0;
    return sprite->rotation;
}

/// @brief Set the clockwise rotation without normalizing its range.
/// @param sprite_ptr Valid sprite handle.
/// @param degrees Signed clockwise rotation in degrees.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_rotation(void *sprite_ptr, int64_t degrees) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Rotation: invalid sprite");
    if (!sprite)
        return;
    sprite->rotation = degrees;
}

/// @brief Get the depth used by scene and batch ordering.
/// @param sprite_ptr Valid sprite handle.
/// @return The stored signed depth; lower values conventionally render behind.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_depth(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Depth: invalid sprite");
    if (!sprite)
        return 0;
    return sprite->depth;
}

/// @brief Set the Z-order depth for back-to-front rendering.
/// @param sprite_ptr Valid sprite handle.
/// @param depth New signed sorting depth.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_depth(void *sprite_ptr, int64_t depth) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Depth: invalid sprite");
    if (!sprite)
        return;
    sprite->depth = depth;
}

/// @brief Get the normalized visibility flag.
/// @param sprite_ptr Valid sprite handle.
/// @return `1` when visible or `0` when drawing and hit testing are disabled.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_visible(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Visible: invalid sprite");
    if (!sprite)
        return 0;
    return sprite->visible;
}

/// @brief Toggle visibility (any non-zero = visible).
/// @param sprite_ptr Valid sprite handle.
/// @param visible Zero to hide the sprite; any nonzero value to show it.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_visible(void *sprite_ptr, int64_t visible) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Visible: invalid sprite");
    if (!sprite)
        return;
    sprite->visible = visible ? 1 : 0;
}

/// @brief Index of the currently displayed animation frame.
/// @param sprite_ptr Valid sprite handle.
/// @return The stored zero-based current-frame index.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_frame(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Frame: invalid sprite");
    if (!sprite)
        return 0;
    return sprite->current_frame;
}

/// @brief Jump to the given frame index, wrapping out-of-range indices modulo frame_count.
/// @details Positive and negative indices wrap when frames exist. The auto-advance
///          timer is reset so animation continues cleanly from the selected frame.
///          A frameless sprite is unchanged.
/// @param sprite_ptr Valid sprite handle.
/// @param frame Requested signed frame index.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_frame(void *sprite_ptr, int64_t frame) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Frame: invalid sprite");
    if (!sprite)
        return;
    if (sprite->frame_count <= 0)
        return;
    frame %= sprite->frame_count;
    if (frame < 0)
        frame += sprite->frame_count;
    sprite->current_frame = frame;
    sprite->last_frame_time = 0; // Reset timer so animation resumes cleanly
}

/// @brief Total number of frames added via `_add_frame` (0 if uninitialized).
/// @param sprite_ptr Valid sprite handle.
/// @return The number of retained frame objects.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_frame_count(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.FrameCount: invalid sprite");
    if (!sprite)
        return 0;
    return sprite->frame_count;
}

/// @brief Horizontal flip flag (1 = mirrored, 0 = normal).
/// @param sprite_ptr Valid sprite handle.
/// @return The normalized horizontal-flip flag.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_flip_x(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.FlipX: invalid sprite");
    if (!sprite)
        return 0;
    return sprite->flip_x;
}

/// @brief Toggle horizontal mirror.
/// @param sprite_ptr Valid sprite handle.
/// @param flip Zero for normal orientation; any nonzero value for mirrored.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_flip_x(void *sprite_ptr, int64_t flip) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.FlipX: invalid sprite");
    if (!sprite)
        return;
    sprite->flip_x = flip ? 1 : 0;
}

/// @brief Vertical flip flag (1 = upside-down, 0 = normal).
/// @param sprite_ptr Valid sprite handle.
/// @return The normalized vertical-flip flag.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
int64_t rt_sprite_get_flip_y(void *sprite_ptr) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.FlipY: invalid sprite");
    if (!sprite)
        return 0;
    return sprite->flip_y;
}

/// @brief Toggle vertical flip.
/// @param sprite_ptr Valid sprite handle.
/// @param flip Zero for normal orientation; any nonzero value for upside-down.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_flip_y(void *sprite_ptr, int64_t flip) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.FlipY: invalid sprite");
    if (!sprite)
        return;
    sprite->flip_y = flip ? 1 : 0;
}

//=============================================================================
// Sprite Methods
//=============================================================================

/// @brief Draw the sprite onto a Canvas with explicit transform overrides.
///
/// Lets callers temporarily override scale/rotation/tint/alpha
/// without mutating the sprite's stored properties — useful for
/// shake effects, hit flashes, or pulsing without tracking
/// previous values. The internal transform pipeline is the same
/// as `rt_sprite_draw`; only the inputs differ.
///
/// Null, stale, wrong-class, invisible, or frameless sprites and null canvases
/// are silently skipped. The simple untransformed path blits the retained frame
/// directly; otherwise the transformed image is borrowed from the sprite cache.
/// @param sprite_ptr Candidate sprite handle; soft-validated.
/// @param canvas_ptr Destination Canvas handle.
/// @param x Destination X coordinate for the transformed origin.
/// @param y Destination Y coordinate for the transformed origin.
/// @param scale_x Temporary horizontal percentage scale.
/// @param scale_y Temporary vertical percentage scale.
/// @param rotation Temporary clockwise rotation in degrees.
/// @param tint_color Temporary tint value, or a negative value for no tint.
/// @param alpha Temporary global alpha multiplier; 255 or greater preserves
///              source alpha and negative values make the result transparent.
void rt_sprite_draw_transformed(void *sprite_ptr,
                                void *canvas_ptr,
                                int64_t x,
                                int64_t y,
                                int64_t scale_x,
                                int64_t scale_y,
                                int64_t rotation,
                                int64_t tint_color,
                                int64_t alpha) {
    if (!sprite_ptr || !canvas_ptr)
        return;

    rt_sprite_impl *sprite = sprite_checked_or_null(sprite_ptr);
    if (!sprite)
        return;

    // Don't draw if not visible
    if (!sprite->visible)
        return;

    void *frame = sprite_get_current_frame_ptr(sprite);
    if (!frame)
        return;

    // If no transform at all, use simple blit
    if (scale_x == 100 && scale_y == 100 && rotation == 0 && !sprite->flip_x && !sprite->flip_y &&
        tint_color < 0 && alpha >= 255) {
        rt_canvas_blit_alpha(canvas_ptr,
                             sprite_sub_saturating(x, sprite->origin_x),
                             sprite_sub_saturating(y, sprite->origin_y),
                             frame);
        return;
    }

    int64_t origin_x = 0;
    int64_t origin_y = 0;
    int8_t origin_centered = 0;
    void *transformed = sprite_prepare_pixels(sprite,
                                              scale_x,
                                              scale_y,
                                              rotation,
                                              tint_color,
                                              alpha,
                                              &origin_x,
                                              &origin_y,
                                              &origin_centered);
    if (!transformed)
        return;

    int64_t blit_x = sprite_sub_saturating(x, origin_x);
    int64_t blit_y = sprite_sub_saturating(y, origin_y);
    if (origin_centered) {
        blit_x = sprite_sub_saturating(x, rt_pixels_width(transformed) / 2);
        blit_y = sprite_sub_saturating(y, rt_pixels_height(transformed) / 2);
    }

    rt_canvas_blit_alpha(canvas_ptr, blit_x, blit_y, transformed);
    /* Do not release `transformed`: sprite_prepare_pixels returns a cache-owned
     * (or frame-borrowed) pointer. The cache is evicted on the next transform-key
     * change and freed in the finalizer. */
}

/// @brief Draw the sprite onto a Canvas using its current properties.
/// @details Uses the stored position, scales, and rotation, with no tint and
///          full global alpha. Invalid handles, invisibility, or absent frames
///          cause a silent no-op.
/// @param sprite_ptr Candidate sprite handle; soft-validated.
/// @param canvas_ptr Destination Canvas handle.
/// @see rt_sprite_draw_transformed
void rt_sprite_draw(void *sprite_ptr, void *canvas_ptr) {
    if (!sprite_ptr)
        return;
    rt_sprite_impl *sprite = sprite_checked_or_null(sprite_ptr);
    if (!sprite)
        return;
    rt_sprite_draw_transformed(sprite_ptr,
                               canvas_ptr,
                               sprite->x,
                               sprite->y,
                               sprite->scale_x,
                               sprite->scale_y,
                               sprite->rotation,
                               -1,
                               255);
}

/// @brief Set the sprite's transform origin (rotation pivot, position anchor).
///
/// Coordinates are in sprite-local pixels at 100% scale and get
/// scaled by the active scale factor at draw time.
/// @param sprite_ptr Valid sprite handle.
/// @param x Unscaled local X coordinate of the destination anchor.
/// @param y Unscaled local Y coordinate of the destination anchor.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_origin(void *sprite_ptr, int64_t x, int64_t y) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.SetOrigin: invalid sprite");
    if (!sprite)
        return;
    sprite->origin_x = x;
    sprite->origin_y = y;
}

/// @brief Append `pixels` as the next animation frame.
///
/// Bumps the Pixels refcount and grows the frame table as needed.
/// Frame indices are assigned in append order starting at 0.
/// @param sprite_ptr Valid sprite handle.
/// @param pixels Valid Pixels object retained as the new final frame.
/// @throws A runtime trap for a null/wrong-class argument or unavailable frame
///         storage.
void rt_sprite_add_frame(void *sprite_ptr, void *pixels) {
    if (!sprite_ptr || !pixels) {
        rt_trap("Sprite.AddFrame: null argument");
        return;
    }
    if (!rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl))) {
        rt_trap("Sprite.AddFrame: invalid pixels");
        return;
    }

    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.AddFrame: invalid sprite");
    if (!sprite)
        return;
    if (!sprite_ensure_frame_capacity(sprite, sprite->frame_count + 1)) {
        rt_trap("Sprite.AddFrame: too many frames");
        return;
    }

    rt_obj_retain_maybe(pixels);
    sprite->frames[sprite->frame_count] = pixels;
    sprite->frame_delays_ms[sprite->frame_count] = sprite->frame_delay_ms;
    sprite->frame_count++;
}

/// @brief Set the per-frame display duration in milliseconds (used by `rt_sprite_update`).
/// @details Updates both the sprite-wide default and every existing frame's
///          individual delay. Nonpositive values are clamped to 1 ms.
/// @param sprite_ptr Valid sprite handle.
/// @param ms Requested delay in milliseconds.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_set_frame_delay(void *sprite_ptr, int64_t ms) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.SetFrameDelay: invalid sprite");
    if (!sprite)
        return;
    ms = sprite_normalize_delay(ms);
    sprite->frame_delay_ms = ms;
    if (sprite->frame_delays_ms) {
        for (int64_t i = 0; i < sprite->frame_count; i++)
            sprite->frame_delays_ms[i] = ms;
    }
}

/// @brief Get one frame's display duration in milliseconds.
/// @param sprite_ptr Valid sprite handle.
/// @param frame Zero-based frame index.
/// @return The normalized positive delay, or `0` after a range trap or when
///         the sprite has no frames.
/// @throws A runtime trap for an invalid sprite or out-of-range frame index.
int64_t rt_sprite_get_frame_delay_at(void *sprite_ptr, int64_t frame) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.GetFrameDelayAt: invalid sprite");
    if (!sprite || sprite->frame_count <= 0)
        return 0;
    if (frame < 0 || frame >= sprite->frame_count) {
        rt_trap("Sprite.GetFrameDelayAt: frame out of range");
        return 0;
    }
    return sprite_frame_delay_at(sprite, frame);
}

/// @brief Set one frame's display duration in milliseconds.
/// @details Nonpositive delays are clamped to 1 ms. The sprite-wide default and
///          all other frame delays remain unchanged.
/// @param sprite_ptr Valid sprite handle.
/// @param frame Zero-based frame index to update.
/// @param ms Requested delay in milliseconds.
/// @throws A runtime trap for an invalid sprite, out-of-range frame, or
///         unavailable delay storage.
void rt_sprite_set_frame_delay_at(void *sprite_ptr, int64_t frame, int64_t ms) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.SetFrameDelayAt: invalid sprite");
    if (!sprite)
        return;
    if (frame < 0 || frame >= sprite->frame_count) {
        rt_trap("Sprite.SetFrameDelayAt: frame out of range");
        return;
    }
    if (!sprite_ensure_frame_capacity(sprite, sprite->frame_count)) {
        rt_trap("Sprite.SetFrameDelayAt: frame delay storage unavailable");
        return;
    }
    sprite->frame_delays_ms[frame] = sprite_normalize_delay(ms);
}

/// @brief Tick the sprite's animation: advance to the next frame if the delay elapsed.
///
/// Reads `rt_timer_ms()` and compares against the last frame
/// transition. Wraps around at `frame_count`. Multi-frame sprites
/// only — single-frame sprites no-op.
///
/// Whole elapsed animation cycles are skipped before at most one pass over the
/// frame table, bounding catch-up work. A backwards clock resets the baseline.
/// Updating is independent of the visibility flag.
/// @param sprite_ptr Candidate sprite handle; null or invalid handles are
///                   silently ignored.
void rt_sprite_update(void *sprite_ptr) {
    if (!sprite_ptr)
        return;

    rt_sprite_impl *sprite = sprite_checked_or_null(sprite_ptr);
    if (!sprite)
        return;
    if (sprite->frame_count <= 1)
        return;

    int64_t now = rt_timer_ms();
    if (sprite->last_frame_time == 0)
        sprite->last_frame_time = now;

    int64_t elapsed = now - sprite->last_frame_time;
    if (elapsed < 0) {
        sprite->last_frame_time = now;
        elapsed = 0;
    }
    int64_t cycle_delay = sprite_cycle_delay(sprite);
    if (cycle_delay > 0 && cycle_delay < INT64_MAX && elapsed >= cycle_delay) {
        int64_t cycles = elapsed / cycle_delay;
        int64_t consumed = cycles > INT64_MAX / cycle_delay ? INT64_MAX : cycles * cycle_delay;
        elapsed = consumed > elapsed ? 0 : elapsed - consumed;
        sprite->last_frame_time = sprite->last_frame_time > INT64_MAX - consumed
                                      ? now
                                      : sprite->last_frame_time + consumed;
    }

    int64_t guard = 0;
    while (guard < sprite->frame_count) {
        int64_t delay = sprite_frame_delay_at(sprite, sprite->current_frame);
        if (elapsed < delay)
            break;
        elapsed -= delay;
        sprite->last_frame_time =
            sprite->last_frame_time > INT64_MAX - delay ? now : sprite->last_frame_time + delay;
        sprite->current_frame = (sprite->current_frame + 1) % sprite->frame_count;
        guard++;
    }
}

/// @brief AABB collision test between two sprites' bounding rectangles.
///
/// Uses each sprite's current width/height after scale (rotation is
/// ignored — this is an axis-aligned test, suitable for broad-phase
/// or simple gameplay collision). Returns 1 if the rectangles
/// overlap, 0 otherwise.
/// @details Position is adjusted by the scaled integer origin. Flip flags do
///          not change the rectangle, and touching edge pixels count as overlap.
///          Invisible, frameless, null, or invalid sprites never overlap.
/// @param sprite_ptr First candidate sprite handle.
/// @param other_ptr Second candidate sprite handle.
/// @return `1` when both scaled origin-adjusted AABBs intersect; otherwise `0`.
int8_t rt_sprite_overlaps(void *sprite_ptr, void *other_ptr) {
    if (!sprite_ptr || !other_ptr)
        return false;

    rt_sprite_impl *s1 = sprite_checked_or_null(sprite_ptr);
    rt_sprite_impl *s2 = sprite_checked_or_null(other_ptr);
    if (!s1 || !s2)
        return false;

    if (!s1->visible || !s2->visible)
        return false;

    // A sprite with no frames has zero extent and no real hitbox — clamping to a
    // 1×1 box below would report phantom collisions, so reject it up front.
    if (rt_sprite_get_width(sprite_ptr) <= 0 || rt_sprite_get_height(sprite_ptr) <= 0 ||
        rt_sprite_get_width(other_ptr) <= 0 || rt_sprite_get_height(other_ptr) <= 0)
        return false;

    // Get bounding boxes
    int64_t w1 = sprite_saturating_scale(rt_sprite_get_width(sprite_ptr), s1->scale_x);
    int64_t h1 = sprite_saturating_scale(rt_sprite_get_height(sprite_ptr), s1->scale_y);
    int64_t w2 = sprite_saturating_scale(rt_sprite_get_width(other_ptr), s2->scale_x);
    int64_t h2 = sprite_saturating_scale(rt_sprite_get_height(other_ptr), s2->scale_y);
    if (w1 < 1)
        w1 = 1;
    if (h1 < 1)
        h1 = 1;
    if (w2 < 1)
        w2 = 1;
    if (h2 < 1)
        h2 = 1;

    int64_t x1 = sprite_sub_saturating(s1->x, sprite_scale_origin(s1->origin_x, s1->scale_x));
    int64_t y1 = sprite_sub_saturating(s1->y, sprite_scale_origin(s1->origin_y, s1->scale_y));
    int64_t x2 = sprite_sub_saturating(s2->x, sprite_scale_origin(s2->origin_x, s2->scale_x));
    int64_t y2 = sprite_sub_saturating(s2->y, sprite_scale_origin(s2->origin_y, s2->scale_y));

    return sprite_interval_overlaps(x1, w1, x2, w2) && sprite_interval_overlaps(y1, h1, y2, h2);
}

/// @brief Point-in-AABB test: is `(px, py)` inside the sprite's bounding rectangle?
/// @details Uses the scaled, origin-adjusted current-frame rectangle and ignores
///          rotation and flips. Boundary pixels are included. Invisible,
///          frameless, null, or invalid sprites return false.
/// @param sprite_ptr Candidate sprite handle.
/// @param px World-space X coordinate to test.
/// @param py World-space Y coordinate to test.
/// @return `1` when the point lies inside the sprite AABB; otherwise `0`.
int8_t rt_sprite_contains(void *sprite_ptr, int64_t px, int64_t py) {
    if (!sprite_ptr)
        return false;

    rt_sprite_impl *sprite = sprite_checked_or_null(sprite_ptr);
    if (!sprite)
        return false;
    if (!sprite->visible)
        return false;

    // Frameless sprites have no real hitbox (see rt_sprite_overlaps).
    if (rt_sprite_get_width(sprite_ptr) <= 0 || rt_sprite_get_height(sprite_ptr) <= 0)
        return false;

    int64_t w = sprite_saturating_scale(rt_sprite_get_width(sprite_ptr), sprite->scale_x);
    int64_t h = sprite_saturating_scale(rt_sprite_get_height(sprite_ptr), sprite->scale_y);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    int64_t x =
        sprite_sub_saturating(sprite->x, sprite_scale_origin(sprite->origin_x, sprite->scale_x));
    int64_t y =
        sprite_sub_saturating(sprite->y, sprite_scale_origin(sprite->origin_y, sprite->scale_y));

    return sprite_interval_contains(x, w, px) && sprite_interval_contains(y, h, py);
}

/// @brief Translate the sprite by `(dx, dy)` pixels.
/// @param sprite_ptr Valid sprite handle.
/// @param dx Signed X displacement.
/// @param dy Signed Y displacement.
/// @throws A runtime trap when @p sprite_ptr is null or not a Sprite.
void rt_sprite_move(void *sprite_ptr, int64_t dx, int64_t dy) {
    rt_sprite_impl *sprite = sprite_checked(sprite_ptr, "Sprite.Move: invalid sprite");
    if (!sprite)
        return;
    sprite->x = sprite_add_saturating(sprite->x, dx);
    sprite->y = sprite_add_saturating(sprite->y, dy);
}

//=============================================================================
// Sprite Animator — Named Animation Clip State Machine
//=============================================================================

// ---------------------------------------------------------------------------
// SpriteAnimator — a separately-allocated controller that drives
// frame transitions on a sprite based on named "clips" (frame
// ranges + per-clip frame delay). Each animator can hold up to
// `MAX_ANIM_CLIPS` clips and plays at most one at a time.
// ---------------------------------------------------------------------------

/// @brief Validate-and-return a SpriteAnimator pointer; returns NULL for NULL or wrong class.
/// @details Soft check (no trap) — used by every public SpriteAnimator entry
///          point so that wrong-class handles are no-ops rather than crashes.
/// @param animator Candidate runtime-managed animator pointer.
/// @return @p animator when its class and size are valid, otherwise `NULL`.
static rt_sprite_animator_t *sprite_animator_checked(rt_sprite_animator_t *animator) {
    if (!animator ||
        !rt_obj_is_instance(animator, RT_SPRITE_ANIMATOR_CLASS_ID, sizeof(rt_sprite_animator_t)))
        return NULL;
    return animator;
}

/// @brief Create a sprite animator that drives multi-clip frame-based animation.
/// @details The new animator has no clips, no active clip, and is stopped. It is
///          bound to a sprite only for each call to `rt_sprite_animator_update()`.
/// @return A caller-owned runtime-managed animator reference, or `NULL` when
///         allocation fails.
rt_sprite_animator_t *rt_sprite_animator_new(void) {
    rt_sprite_animator_t *anim = (rt_sprite_animator_t *)rt_obj_new_i64(
        RT_SPRITE_ANIMATOR_CLASS_ID, (int64_t)sizeof(rt_sprite_animator_t));
    if (!anim)
        return NULL;
    memset(anim, 0, sizeof(rt_sprite_animator_t));
    anim->current_clip = -1;
    anim->playing = 0;
    return anim;
}

/// @brief Release one reference to a sprite animator.
/// @details The runtime frees the object only when this release reaches zero.
///          Clip names are fixed arrays stored inside the animator and require
///          no separate deallocation. Null or invalid handles are ignored.
/// @param animator Animator reference to release.
void rt_sprite_animator_destroy(rt_sprite_animator_t *animator) {
    animator = sprite_animator_checked(animator);
    if (!animator)
        return;
    if (rt_obj_release_check0(animator))
        rt_obj_free(animator);
}

/// @brief Register a named animation clip (frame range + per-frame delay + loop flag).
///
/// Clip names are matched case-sensitively in `play`. Adding a duplicate name
/// replaces the existing clip in-place. Start is clamped to zero, frame count
/// to at least one, and nonpositive delay to 100 ms; loop is stored verbatim
/// and interpreted by truthiness.
/// @param animator Candidate animator handle.
/// @param name NUL-terminated clip name shorter than 64 bytes.
/// @param start_frame First sprite frame in the clip.
/// @param frame_count Requested number of consecutive frames.
/// @param frame_delay_ms Delay between clip frames in milliseconds.
/// @param loop Zero for play-once behavior; nonzero to loop.
/// @return `1` on success; `0` for an invalid animator/name, an overlong name,
///         or a full clip table when no existing clip can be replaced.
int rt_sprite_animator_add_clip(rt_sprite_animator_t *animator,
                                const char *name,
                                int64_t start_frame,
                                int64_t frame_count,
                                int64_t frame_delay_ms,
                                int loop) {
    animator = sprite_animator_checked(animator);
    if (!animator || !name)
        return 0;
    if (strlen(name) >= sizeof(animator->clips[0].name))
        return 0;

    int existing = -1;
    for (int i = 0; i < animator->clip_count; i++) {
        if (strcmp(animator->clips[i].name, name) == 0) {
            existing = i;
            break;
        }
    }
    if (existing < 0 && animator->clip_count >= RT_ANIM_MAX_CLIPS)
        return 0;

    rt_anim_clip_t *clip =
        existing >= 0 ? &animator->clips[existing] : &animator->clips[animator->clip_count++];
    /* Safe string copy: name field is char[64], guarantee NUL termination */
    int i = 0;
    while (i < 63 && name[i]) {
        clip->name[i] = name[i];
        i++;
    }
    clip->name[i] = '\0';
    clip->start_frame = start_frame < 0 ? 0 : start_frame;
    clip->frame_count = frame_count > 0 ? frame_count : 1;
    clip->frame_delay_ms = frame_delay_ms > 0 ? frame_delay_ms : 100;
    clip->loop = loop;
    return 1;
}

/// @brief Begin playing the clip with the given name from its first frame.
///
/// Resets timing so the next `update` advances based on the clip's
/// `frame_delay_ms`. If the named clip doesn't exist, returns 0
/// and leaves the previously-playing clip (if any) running.
/// @param animator Candidate animator handle.
/// @param name Case-sensitive NUL-terminated clip name.
/// @return `1` when the clip is found and started; otherwise `0`.
int8_t rt_sprite_animator_play(rt_sprite_animator_t *animator, const char *name) {
    animator = sprite_animator_checked(animator);
    if (!animator || !name)
        return 0;

    for (int i = 0; i < animator->clip_count; i++) {
        /* Simple string comparison (no strncmp dependency on all platforms) */
        const char *a = animator->clips[i].name;
        const char *b = name;
        int match = 1;
        while (*a || *b) {
            if (*a != *b) {
                match = 0;
                break;
            }
            a++;
            b++;
        }
        if (match) {
            animator->current_clip = i;
            animator->clip_frame = 0;
            animator->last_update_ms = 0; /* reset on next update */
            animator->playing = 1;
            return 1;
        }
    }
    return 0; /* clip not found */
}

/// @brief Stop the active clip; subsequent `update` calls become no-ops until `play`.
/// @details The current clip index and frame offset remain stored, but queries
///          report no current clip while `playing` is zero.
/// @param animator Candidate animator handle; invalid handles are ignored.
void rt_sprite_animator_stop(rt_sprite_animator_t *animator) {
    animator = sprite_animator_checked(animator);
    if (!animator)
        return;
    animator->playing = 0;
}

/// @brief Tick the animator and write the active clip's current frame to the sprite.
///
/// Drives forward by the elapsed wall-clock time since the last
/// transition. On reaching the end of a non-looping clip, holds
/// the final frame and stops; looping clips wrap to `start_frame`.
/// @details The clip range is truncated to the sprite's available frames. A
///          missing range stops playback. Elapsed time may advance multiple
///          frames in one call, and a backwards clock resets the time baseline.
/// @param animator Candidate animator handle.
/// @param sprite_ptr Sprite whose current frame should be driven. A nonnull
///                   wrong-class handle may trap through strict sprite accessors.
void rt_sprite_animator_update(rt_sprite_animator_t *animator, void *sprite_ptr) {
    animator = sprite_animator_checked(animator);
    if (!animator || !animator->playing || animator->current_clip < 0 ||
        animator->current_clip >= animator->clip_count || !sprite_ptr)
        return;

    rt_anim_clip_t *clip = &animator->clips[animator->current_clip];
    int64_t sprite_frames = rt_sprite_get_frame_count(sprite_ptr);
    if (sprite_frames <= 0 || clip->start_frame >= sprite_frames) {
        animator->playing = 0;
        animator->clip_frame = 0;
        return;
    }
    int64_t effective_count = clip->frame_count;
    int64_t available = sprite_frames - clip->start_frame;
    if (effective_count > available)
        effective_count = available;
    if (effective_count <= 0) {
        animator->playing = 0;
        animator->clip_frame = 0;
        return;
    }
    if (animator->clip_frame < 0)
        animator->clip_frame = 0;
    if (animator->clip_frame >= effective_count)
        animator->clip_frame =
            clip->loop ? animator->clip_frame % effective_count : effective_count - 1;

    int64_t now = rt_timer_ms();

    if (animator->last_update_ms == 0)
        animator->last_update_ms = now;

    int64_t elapsed = now - animator->last_update_ms;
    if (elapsed < 0) {
        animator->last_update_ms = now;
        elapsed = 0;
    }
    if (elapsed >= clip->frame_delay_ms) {
        /* Advance one frame (may be multiple if behind) */
        int64_t steps = elapsed / clip->frame_delay_ms;
        int64_t consumed =
            steps > INT64_MAX / clip->frame_delay_ms ? INT64_MAX : steps * clip->frame_delay_ms;
        animator->last_update_ms = animator->last_update_ms > INT64_MAX - consumed
                                       ? now
                                       : animator->last_update_ms + consumed;

        if (clip->loop) {
            steps %= effective_count;
            animator->clip_frame = (animator->clip_frame + steps) % effective_count;
        } else {
            if (steps >= effective_count - animator->clip_frame) {
                /* Play-once: hold on last frame */
                animator->clip_frame = effective_count - 1;
                animator->playing = 0;
            } else {
                animator->clip_frame += steps;
            }
        }
    }

    /* Update the sprite's current frame */
    if (clip->start_frame <= INT64_MAX - animator->clip_frame)
        rt_sprite_set_frame(sprite_ptr, clip->start_frame + animator->clip_frame);
}

/// @brief True if a clip is currently playing (was started and hasn't reached a non-loop end).
/// @param animator Candidate animator handle.
/// @return `1` when playback is active; `0` for stopped or invalid animators.
int8_t rt_sprite_animator_is_playing(rt_sprite_animator_t *animator) {
    animator = sprite_animator_checked(animator);
    return (animator && animator->playing) ? 1 : 0;
}

/// @brief Name of the active clip, or NULL when nothing is playing.
/// @param animator Candidate animator handle.
/// @return A borrowed pointer to the active clip's internal NUL-terminated name,
///         or `NULL` while stopped or invalid. The pointer remains valid only
///         while the animator exists and that clip is not replaced.
const char *rt_sprite_animator_get_current(rt_sprite_animator_t *animator) {
    animator = sprite_animator_checked(animator);
    if (!animator || !animator->playing || animator->current_clip < 0 ||
        animator->current_clip >= animator->clip_count)
        return NULL;
    return animator->clips[animator->current_clip].name;
}

/// @brief Zia/BASIC bridge: `add_clip` taking a Zanna `rt_string` for the name.
/// @param animator Candidate opaque SpriteAnimator handle.
/// @param name Borrowed runtime string containing the clip name.
/// @param start_frame First sprite frame in the clip.
/// @param frame_count Requested number of consecutive frames.
/// @param frame_delay_ms Delay between clip frames in milliseconds.
/// @param loop Zero for play-once behavior; nonzero to loop.
/// @return `1` when the clip is registered; otherwise `0`.
/// @see rt_sprite_animator_add_clip
int8_t rt_sprite_animator_add_clip_str(void *animator,
                                       rt_string name,
                                       int64_t start_frame,
                                       int64_t frame_count,
                                       int64_t frame_delay_ms,
                                       int64_t loop) {
    const char *clipName = name ? rt_string_cstr(name) : NULL;
    int ok = rt_sprite_animator_add_clip((rt_sprite_animator_t *)animator,
                                         clipName,
                                         start_frame,
                                         frame_count,
                                         frame_delay_ms,
                                         loop != 0);
    return ok ? 1 : 0;
}

/// @brief Zia/BASIC bridge: `play` taking a Zanna `rt_string`.
/// @param animator Candidate opaque SpriteAnimator handle.
/// @param name Borrowed runtime string containing the case-sensitive clip name.
/// @return `1` when the named clip starts; otherwise `0`.
int8_t rt_sprite_animator_play_str(void *animator, rt_string name) {
    const char *clipName = name ? rt_string_cstr(name) : NULL;
    int8_t ok = rt_sprite_animator_play((rt_sprite_animator_t *)animator, clipName);
    return ok;
}

/// @brief Zia/BASIC bridge: `get_current` returning the active clip name as `rt_string`.
/// @details Returns the shared empty runtime string when nothing is playing;
///          otherwise constructs a runtime string from the animator's internal name.
/// @param animator Candidate opaque SpriteAnimator handle.
/// @return Runtime string containing the current clip name, or the empty string.
rt_string rt_sprite_animator_get_current_str(void *animator) {
    const char *name = rt_sprite_animator_get_current((rt_sprite_animator_t *)animator);
    if (!name)
        return rt_str_empty();
    return rt_string_from_bytes(name, strlen(name));
}
