//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_graphics_internal.h
/// @brief Defines shared Canvas internals and overflow-safe graphics helpers.
///
/// @details
/// This subsystem-private header centralizes the canvas representation,
/// coordinate scaling, clipping, color conversion, and checked arithmetic
/// required by the split Canvas implementation units.
///
// File: src/runtime/graphics/common/rt_graphics_internal.h
// Purpose: Shared internal definitions for the rt_graphics subsystem. Provides
//   the rt_canvas struct, the rt_pixels_impl forward declaration, common
//   includes, and static helper functions used across rt_canvas.c,
//   rt_drawing.c, and rt_drawing_advanced.c.
//
// Key invariants:
//   - This header is internal to the graphics subsystem; never include from
//     outside src/runtime/graphics/.
//   - All helpers are declared static inline to avoid duplicate-symbol errors
//     when included from multiple translation units.
//   - The rt_canvas struct layout must remain identical across all files.
//
// Ownership/Lifetime:
//   - No allocations; struct definitions and pure helper functions only.
//
// Links: rt_graphics.h (public API), rt_canvas.c, rt_drawing.c,
//        rt_drawing_advanced.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_graphics.h"
#include "rt_pixels_internal.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Static inline helpers — available to all translation units that include this
//=============================================================================

/// @brief Absolute value for int64_t.
/// @param x Signed input value.
/// @return `abs(x)`, with `INT64_MIN` saturated to `INT64_MAX`.
static inline int64_t rtg_abs64(int64_t x) {
    if (x == INT64_MIN)
        return INT64_MAX;
    return x < 0 ? -x : x;
}

/// @brief Minimum of two int64_t values.
/// @param a First value.
/// @param b Second value.
/// @return The lesser value.
static inline int64_t rtg_min64(int64_t a, int64_t b) {
    return a < b ? a : b;
}

/// @brief Maximum of two int64_t values.
/// @param a First value.
/// @param b Second value.
/// @return The greater value.
static inline int64_t rtg_max64(int64_t a, int64_t b) {
    return a > b ? a : b;
}

/// @brief Saturating int64 addition for bounds math.
/// @param a Left operand.
/// @param b Right operand.
/// @return The mathematical sum clamped to the `int64_t` range.
static inline int64_t rtg_add_sat64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Saturating int64 subtraction for coordinate math.
/// @param a Minuend.
/// @param b Subtrahend.
/// @return The mathematical difference clamped to the `int64_t` range.
static inline int64_t rtg_sub_sat64(int64_t a, int64_t b) {
    if (b > 0 && a < INT64_MIN + b)
        return INT64_MIN;
    if (b < 0 && a > INT64_MAX + b)
        return INT64_MAX;
    return a - b;
}

/// @brief Saturating subtract by a non-negative amount.
/// @param a Minuend.
/// @param b Non-negative amount to subtract; non-positive values leave @p a unchanged.
/// @return The difference saturated at `INT64_MIN`.
static inline int64_t rtg_sub_nonneg_sat64(int64_t a, int64_t b) {
    if (b <= 0)
        return a;
    if (a < INT64_MIN + b)
        return INT64_MIN;
    return a - b;
}

/// @brief Saturating int64 multiplication for coordinate math.
/// @param a Left operand.
/// @param b Right operand.
/// @return The mathematical product clamped to the `int64_t` range.
static inline int64_t rtg_mul_sat64(int64_t a, int64_t b) {
    if (a == 0 || b == 0)
        return 0;
    if (a == -1)
        return b == INT64_MIN ? INT64_MAX : -b;
    if (b == -1)
        return a == INT64_MIN ? INT64_MAX : -a;
    if (a > 0) {
        if (b > 0 && a > INT64_MAX / b)
            return INT64_MAX;
        if (b < 0 && b < INT64_MIN / a)
            return INT64_MIN;
    } else {
        if (b > 0 && a < INT64_MIN / b)
            return INT64_MIN;
        if (b < 0 && a < INT64_MAX / b)
            return INT64_MAX;
    }
    return a * b;
}

/// @brief Clamp an int64 to the int32 range accepted by ZannaGFX.
/// @param value Input value.
/// @return @p value clamped to `INT32_MIN..INT32_MAX`.
static inline int32_t rtg_clamp_i64_to_i32(int64_t value) {
    if (value > INT32_MAX)
        return INT32_MAX;
    if (value < INT32_MIN)
        return INT32_MIN;
    return (int32_t)value;
}

/// @brief Return whether an int64 can be passed to ZannaGFX without narrowing.
/// @param value Input value.
/// @return Non-zero when @p value lies in the signed 32-bit range.
static inline int8_t rtg_i64_fits_i32(int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX;
}

/// @brief Number of leading elements to skip when a span starts at a negative
///        coordinate.
/// @details If @p start >= 0 or @p len <= 0 nothing is skipped. Otherwise the
///          skip is min(-start, len), saturating the INT64_MIN edge case so the
///          caller can clip a copy span to the non-negative region.
/// @param start Span start coordinate.
/// @param len Span length.
/// @return Count in [0, len] of elements to drop from the front of the span.
static inline int64_t rtg_negative_skip(int64_t start, int64_t len) {
    if (start >= 0 || len <= 0)
        return 0;
    if (start == INT64_MIN)
        return len;
    int64_t skip = -start;
    return skip >= len ? len : skip;
}

/// @brief Clip a 1-D copy span against both source and destination bounds.
/// @details Adjusts @p dst, @p src and @p len in place so that the resulting
///          span lies fully within [0, dst_limit) on the destination and
///          [0, src_limit) on the source, accounting for negative start
///          coordinates on either side (via rtg_negative_skip). On any
///          degenerate input @p len is set to 0.
/// @param dst_limit Destination extent (must be > 0 to copy).
/// @param src_limit Source extent (must be > 0 to copy).
/// @param dst In/out destination start coordinate.
/// @param src In/out source start coordinate.
/// @param len In/out span length.
/// @return Non-zero if a non-empty clipped span remains, 0 otherwise.
static inline int8_t rtg_clip_copy_axis(
    int64_t dst_limit, int64_t src_limit, int64_t *dst, int64_t *src, int64_t *len) {
    if (!dst || !src || !len || *len <= 0 || dst_limit <= 0 || src_limit <= 0) {
        if (len)
            *len = 0;
        return 0;
    }

    int64_t skip = rtg_negative_skip(*src, *len);
    if (skip >= *len) {
        *len = 0;
        return 0;
    }
    if (skip > 0) {
        if (*dst > INT64_MAX - skip) {
            *len = 0;
            return 0;
        }
        *dst += skip;
        *src = 0;
        *len -= skip;
    }
    if (*src >= src_limit) {
        *len = 0;
        return 0;
    }
    int64_t src_remaining = src_limit - *src;
    if (*len > src_remaining)
        *len = src_remaining;

    skip = rtg_negative_skip(*dst, *len);
    if (skip >= *len) {
        *len = 0;
        return 0;
    }
    if (skip > 0) {
        if (*src > INT64_MAX - skip) {
            *len = 0;
            return 0;
        }
        *src += skip;
        *dst = 0;
        *len -= skip;
    }
    if (*dst >= dst_limit) {
        *len = 0;
        return 0;
    }
    int64_t dst_remaining = dst_limit - *dst;
    if (*len > dst_remaining)
        *len = dst_remaining;
    return *len > 0;
}

/// @brief Sine in degrees as fixed-point Q10.
/// @param deg Angle in degrees; values wrap modulo 360.
/// @return sin(deg) * 1024 for fixed-point precision.
static inline int64_t rtg_sin_deg_fp(int64_t deg) {
    deg = deg % 360;
    if (deg < 0)
        deg += 360;
    double radians = (double)deg * (3.14159265358979323846 / 180.0);
    return (int64_t)llround(sin(radians) * 1024.0);
}

/// @brief Cosine in degrees as fixed-point Q10.
/// @param deg Angle in degrees; values wrap modulo 360.
/// @return cos(deg) * 1024 for fixed-point precision.
static inline int64_t rtg_cos_deg_fp(int64_t deg) {
    deg %= 360;
    return rtg_sin_deg_fp(deg + 90);
}

/// @brief Convert RGB (0-255 each) to HSL.
/// @param r Red channel in 0..255.
/// @param g Green channel in 0..255.
/// @param b Blue channel in 0..255.
/// @param h Output hue (0-360).
/// @param s Output saturation (0-100).
/// @param l Output lightness (0-100).
static inline void rtg_rgb_to_hsl(
    int64_t r, int64_t g, int64_t b, int64_t *h, int64_t *s, int64_t *l) {
    int64_t max_c = (r > g) ? (r > b ? r : b) : (g > b ? g : b);
    int64_t min_c = (r < g) ? (r < b ? r : b) : (g < b ? g : b);
    int64_t delta = max_c - min_c;

    int64_t sum = max_c + min_c;
    *l = sum * 100 / 510;

    if (delta == 0) {
        *h = 0;
        *s = 0;
    } else {
        int64_t saturation_denominator = sum <= 255 ? sum : 510 - sum;
        *s = saturation_denominator > 0 ? delta * 100 / saturation_denominator : 0;

        if (max_c == r)
            *h = ((g - b) * 60 / delta + 360) % 360;
        else if (max_c == g)
            *h = (b - r) * 60 / delta + 120;
        else
            *h = (r - g) * 60 / delta + 240;

        if (*h < 0)
            *h += 360;
    }
}

/// @brief Hue to RGB conversion helper for HSL.
/// @param p Lower intermediate HSL channel value.
/// @param q Upper intermediate HSL channel value.
/// @param t Hue offset in degrees; wraps modulo 360.
/// @return Interpolated channel value in the helper's 0..10000 scale.
static inline int64_t rtg_hue_to_rgb_helper(int64_t p, int64_t q, int64_t t) {
    t %= 360;
    if (t < 0)
        t += 360;
    if (t < 60)
        return p + (q - p) * t / 60;
    if (t < 180)
        return q;
    if (t < 240)
        return p + (q - p) * (240 - t) / 60;
    return p;
}

/// @brief Convert HSL to RGB.
/// @param h Hue (0-360).
/// @param s Saturation (0-100).
/// @param l Lightness (0-100).
/// @param r Output red (0-255).
/// @param g Output green (0-255).
/// @param b Output blue (0-255).
static inline void rtg_hsl_to_rgb(
    int64_t h, int64_t s, int64_t l, int64_t *r, int64_t *g, int64_t *b) {
    if (s == 0) {
        *r = *g = *b = l * 255 / 100;
        return;
    }

    /* Keep two decimal places of percentage precision through q/p and hue
     * interpolation. The prior 0..100 intermediates quantized channels before
     * their final 8-bit conversion. */
    int64_t q = (l < 50) ? l * (100 + s) : l * 100 + s * 100 - l * s;
    int64_t p = 2 * l * 100 - q;

    *r = rtg_hue_to_rgb_helper(p, q, h + 120) * 255 / 10000;
    *g = rtg_hue_to_rgb_helper(p, q, h) * 255 / 10000;
    *b = rtg_hue_to_rgb_helper(p, q, h - 120) * 255 / 10000;

    if (*r < 0)
        *r = 0;
    if (*r > 255)
        *r = 255;
    if (*g < 0)
        *g = 0;
    if (*g > 255)
        *g = 255;
    if (*b < 0)
        *b = 0;
    if (*b > 255)
        *b = 255;
}

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_action.h"
#include "rt_font.h"
#include "rt_input.h"
#include "rt_object.h"
#include "rt_pixels.h"

#include "vgfx.h"

/// @brief Validate a direct-access RGBA framebuffer descriptor.
/// @details Rejects missing storage, non-positive extents, and inconsistent
///          strides before runtime drawing code performs pointer arithmetic.
/// @param fb Borrowed descriptor returned by `vgfx_get_framebuffer`.
/// @return Non-zero when every addressable row has exactly `width * 4` bytes.
static inline int8_t rtg_framebuffer_is_valid(const vgfx_framebuffer_t *fb) {
    if (!fb || !fb->pixels || fb->width <= 0 || fb->height <= 0)
        return 0;
    int64_t required_stride = (int64_t)fb->width * 4;
    return required_stride <= INT32_MAX && fb->stride == (int32_t)required_stride;
}

/* Internal input teardown helpers used by canvas lifecycle code. */
/// @brief Drop any keyboard-input state bound to @p canvas if it is the
///        currently-tracked canvas (called when a Canvas is destroyed).
/// @param canvas Canvas handle being destroyed.
void rt_keyboard_clear_canvas_if_matches(void *canvas);
/// @brief Drop any mouse-input state bound to @p canvas if it is the
///        currently-tracked canvas (called when a Canvas is destroyed).
/// @param canvas Canvas handle being destroyed.
void rt_mouse_clear_canvas_if_matches(void *canvas);

/// @brief Magic value used to reject accidental calls with non-Canvas objects.
#define RT_CANVAS_MAGIC 0x564950455243414eLL /* "ZANNACAN" */

/// @brief Internal canvas wrapper structure.
/// @details Contains the vptr (for future OOP support), a runtime magic guard,
///   and the vgfx window handle.
typedef struct {
    void *vptr;              ///< VTable pointer (reserved for future use)
    int64_t magic;           ///< RT_CANVAS_MAGIC while this object is a live Canvas
    vgfx_window_t gfx_win;   ///< ZannaGFX window handle
    int64_t should_close;    ///< Close request flag
    vgfx_event_t last_event; ///< Last polled event for retrieval
    char *title;             ///< Cached window title (heap-allocated, freed in finalizer)
    size_t title_len;        ///< Byte length of cached title; permits embedded NULs in GetTitle()
    int64_t logical_width;   ///< Canvas drawing width in logical pixels
    int64_t logical_height;  ///< Canvas drawing height in logical pixels
    int64_t last_flip_us;    ///< Monotonic time (microseconds) of last Flip()
    int64_t delta_time_ms;   ///< Milliseconds elapsed between the last two Flip() calls
    int64_t dt_max_ms;       ///< Maximum delta time clamp (0 = no clamping)
    int8_t clip_enabled;     ///< Runtime mirror of the logical clip rect state
    int64_t clip_x;          ///< Logical clip X
    int64_t clip_y;          ///< Logical clip Y
    int64_t clip_w;          ///< Logical clip width
    int64_t clip_h;          ///< Logical clip height
    int8_t relative_mouse_applied; ///< Relative (raw) mouse mode currently applied to the window
    int8_t window_state_synced;    ///< Cached scale/clip state has been applied to gfx_win
    int8_t applied_clip_enabled;   ///< Clip-enabled value last applied to gfx_win
    float applied_coord_scale;     ///< Coordinate scale last applied to gfx_win
    int64_t applied_clip_x;        ///< Logical clip X last applied to gfx_win
    int64_t applied_clip_y;        ///< Logical clip Y last applied to gfx_win
    int64_t applied_clip_w;        ///< Logical clip width last applied to gfx_win
    int64_t applied_clip_h;        ///< Logical clip height last applied to gfx_win
} rt_canvas;

/// @brief Safely down-cast an opaque pointer to rt_canvas.
/// @details Validates the object is a live Canvas instance (correct class id,
///          size, and RT_CANVAS_MAGIC guard) before returning it.
/// @param canvas_ptr Candidate opaque runtime object.
/// @return The rt_canvas pointer, or NULL if @p canvas_ptr is not a Canvas.
static inline rt_canvas *rt_canvas_checked(void *canvas_ptr) {
    if (!canvas_ptr)
        return NULL;
    if (!rt_obj_is_instance(canvas_ptr, RT_CANVAS_CLASS_ID, sizeof(rt_canvas)))
        return NULL;
    rt_canvas *canvas = (rt_canvas *)canvas_ptr;
    return canvas->magic == RT_CANVAS_MAGIC ? canvas : NULL;
}

#define RT_COLOR_EXPLICIT_ALPHA_FLAG ((int64_t)1 << 56)

/// @brief Clamp a HiDPI scale factor to the sane range 1.0 through 16.0.
/// @details Guards against zero, negative, non-finite, and absurd scales
///          reported by a window backend, which would otherwise blow up
///          logical-to-physical math.
/// @param scale Backend-reported scale factor.
/// @return Finite @p scale clamped to `[1.0, 16.0]`.
static inline float rtg_sanitize_scale(float scale) {
    if (!isfinite((double)scale) || scale < 1.0f)
        return 1.0f;
    return scale > 16.0f ? 16.0f : scale;
}

/// @brief Round a double to the nearest int64 (half away from zero).
/// @details Finite out-of-range values saturate; NaN maps to zero so conversion
///          never invokes undefined floating-to-integer behavior.
/// @param value Floating-point value to convert.
/// @return The rounded value saturated to the `int64_t` range.
static inline int64_t rtg_round_scaled(double value) {
    if (isnan(value))
        return 0;
    if (value >= (double)INT64_MAX)
        return INT64_MAX;
    if (value <= (double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

/// @brief Convert a logical (DPI-independent) coordinate to a physical pixel
///        coordinate by multiplying by the sanitized HiDPI scale.
/// @param logical Logical-pixel coordinate.
/// @param scale HiDPI scale factor.
/// @return Rounded, saturating physical-pixel coordinate.
static inline int64_t rtg_scale_up_i64(int64_t logical, float scale) {
    return rtg_round_scaled((double)logical * (double)rtg_sanitize_scale(scale));
}

/// @brief Convert a physical pixel coordinate back to a logical coordinate by
///        dividing by the sanitized HiDPI scale (inverse of rtg_scale_up_i64).
/// @param physical Physical-pixel coordinate.
/// @param scale HiDPI scale factor.
/// @return Rounded, saturating logical-pixel coordinate.
static inline int64_t rtg_scale_down_i64(int64_t physical, float scale) {
    return rtg_round_scaled((double)physical / (double)rtg_sanitize_scale(scale));
}

/// @brief Return the coordinate scale the Canvas should use for drawing/input.
/// @details Windowed Canvas drawing uses the platform HiDPI scale. In native
///          fullscreen, ZannaGFX resizes the framebuffer to the monitor; Canvas
///          keeps its designed logical size and scales draw/input coordinates to
///          that fullscreen framebuffer instead of exposing the monitor as a new
///          game resolution.
/// @param canvas Canvas whose current window and logical size are inspected.
/// @return Effective coordinate scale, constrained to `1.0..16.0`.
static inline float rt_canvas_effective_coord_scale(rt_canvas *canvas) {
    if (!canvas || !canvas->gfx_win)
        return 1.0f;

    float scale = rtg_sanitize_scale(vgfx_window_get_scale(canvas->gfx_win));
    if (vgfx_is_fullscreen(canvas->gfx_win) != 1)
        return scale;
    if (canvas->logical_width <= 0 || canvas->logical_height <= 0)
        return scale;

    int32_t framebuffer_w = vgfx_window_get_width(canvas->gfx_win);
    int32_t framebuffer_h = vgfx_window_get_height(canvas->gfx_win);
    if (framebuffer_w <= 0 || framebuffer_h <= 0)
        return scale;

    double sx = (double)framebuffer_w / (double)canvas->logical_width;
    double sy = (double)framebuffer_h / (double)canvas->logical_height;
    double presentation_scale = sx < sy ? sx : sy;
    if (presentation_scale < 1.0)
        return scale;
    if (presentation_scale > 16.0)
        return 16.0f;
    return (float)presentation_scale;
}

/// @brief Push the canvas's logical coordinate scale and clip rect into the
///        underlying ZannaGFX window.
/// @details Re-reads the window HiDPI scale and applies coordinate-scale/clip
///          changes only when they differ from the cached backend state. Scale
///          changes force clip reapplication because the backend stores the
///          corresponding physical bounds. No-op when the canvas has no window.
/// @param canvas Canvas whose scale and clip state are synchronized.
static inline void rt_canvas_resync_window_state(rt_canvas *canvas) {
    if (!canvas || !canvas->gfx_win)
        return;

    float scale = rt_canvas_effective_coord_scale(canvas);
    int8_t scale_changed = !canvas->window_state_synced || scale != canvas->applied_coord_scale;
    int8_t clip_changed =
        scale_changed || canvas->clip_enabled != canvas->applied_clip_enabled ||
        (canvas->clip_enabled &&
         (canvas->clip_x != canvas->applied_clip_x || canvas->clip_y != canvas->applied_clip_y ||
          canvas->clip_w != canvas->applied_clip_w || canvas->clip_h != canvas->applied_clip_h));
    if (scale_changed)
        vgfx_set_coord_scale(canvas->gfx_win, scale);
    if (clip_changed) {
        if (canvas->clip_enabled) {
            vgfx_set_clip(canvas->gfx_win,
                          rtg_clamp_i64_to_i32(canvas->clip_x),
                          rtg_clamp_i64_to_i32(canvas->clip_y),
                          rtg_clamp_i64_to_i32(canvas->clip_w),
                          rtg_clamp_i64_to_i32(canvas->clip_h));
        } else {
            vgfx_clear_clip(canvas->gfx_win);
        }
    }
    canvas->applied_coord_scale = scale;
    canvas->applied_clip_enabled = canvas->clip_enabled;
    canvas->applied_clip_x = canvas->clip_x;
    canvas->applied_clip_y = canvas->clip_y;
    canvas->applied_clip_w = canvas->clip_w;
    canvas->applied_clip_h = canvas->clip_h;
    canvas->window_state_synced = 1;
}

/// @brief Compute the effective logical clip rectangle for a canvas.
/// @details Resyncs window state, then intersects the canvas's logical clip
///          (or the full window if clipping is disabled) with the window
///          bounds. Outputs are written to @p x/@p y/@p w/@p h.
/// @param canvas Canvas whose logical clip region is queried.
/// @param x Output receiving the clipped left coordinate.
/// @param y Output receiving the clipped top coordinate.
/// @param w Output receiving the clipped width.
/// @param h Output receiving the clipped height.
/// @return Non-zero if a non-empty clip region results, 0 if fully clipped out.
static inline int8_t rt_canvas_get_logical_clip_bounds(
    rt_canvas *canvas, int64_t *x, int64_t *y, int64_t *w, int64_t *h) {
    if (!canvas || !canvas->gfx_win || !x || !y || !w || !h)
        return 0;

    rt_canvas_resync_window_state(canvas);

    int32_t canvas_w = 0;
    int32_t canvas_h = 0;
    vgfx_get_size(canvas->gfx_win, &canvas_w, &canvas_h);

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = (int64_t)canvas_w;
    int64_t clip_h = (int64_t)canvas_h;
    if (canvas->clip_enabled) {
        clip_x = canvas->clip_x;
        clip_y = canvas->clip_y;
        clip_w = canvas->clip_w;
        clip_h = canvas->clip_h;
    }

    int64_t x0 = rtg_max64(0, clip_x);
    int64_t y0 = rtg_max64(0, clip_y);
    int64_t x1 = rtg_min64((int64_t)canvas_w, rtg_add_sat64(clip_x, clip_w));
    int64_t y1 = rtg_min64((int64_t)canvas_h, rtg_add_sat64(clip_y, clip_h));
    if (clip_w <= 0 || clip_h <= 0 || x1 <= x0 || y1 <= y0) {
        *x = x0;
        *y = y0;
        *w = 0;
        *h = 0;
        return 0;
    }

    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return 1;
}

/// @brief Intersect a caller-supplied logical rect with the canvas clip region.
/// @details Clamps the in/out rect (@p x, @p y, @p w, @p h) to the effective
///          logical clip bounds from rt_canvas_get_logical_clip_bounds(). On
///          empty input or no overlap, the size outputs are zeroed.
/// @param canvas Canvas whose clip region constrains the rectangle.
/// @param x In/out rectangle left coordinate.
/// @param y In/out rectangle top coordinate.
/// @param w In/out rectangle width.
/// @param h In/out rectangle height.
/// @return Non-zero if a non-empty intersection remains, 0 otherwise.
static inline int8_t rt_canvas_clip_intersect_logical(
    rt_canvas *canvas, int64_t *x, int64_t *y, int64_t *w, int64_t *h) {
    if (!x || !y || !w || !h || *w <= 0 || *h <= 0)
        return 0;

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h)) {
        *w = 0;
        *h = 0;
        return 0;
    }

    int64_t x0 = rtg_max64(*x, clip_x);
    int64_t y0 = rtg_max64(*y, clip_y);
    int64_t x1 = rtg_min64(rtg_add_sat64(*x, *w), rtg_add_sat64(clip_x, clip_w));
    int64_t y1 = rtg_min64(rtg_add_sat64(*y, *h), rtg_add_sat64(clip_y, clip_h));
    if (x1 <= x0 || y1 <= y0) {
        *x = x0;
        *y = y0;
        *w = 0;
        *h = 0;
        return 0;
    }

    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return 1;
}

#endif /* ZANNA_ENABLE_GRAPHICS */

/// @brief Blit a sub-rectangle of @p pixels_ptr to (dx,dy) with straight-alpha
///        compositing (internal; defined in rt_drawing.c). Region-cropped, blending
///        counterpart of rt_canvas_blit_region — used by the SpriteBatch fast path so
///        transparent sprite-sheet frames blend instead of overwriting. Declared
///        outside the ZANNA_ENABLE_GRAPHICS guard so translation units compiled
///        without graphics (e.g. isolated contract tests that stub it) still see it.
/// @param canvas_ptr Destination Canvas handle.
/// @param dx Destination X coordinate.
/// @param dy Destination Y coordinate.
/// @param pixels_ptr Source Pixels handle.
/// @param sx Source-region X coordinate.
/// @param sy Source-region Y coordinate.
/// @param w Source-region width.
/// @param h Source-region height.
void rt_canvas_blit_region_alpha(void *canvas_ptr,
                                 int64_t dx,
                                 int64_t dy,
                                 void *pixels_ptr,
                                 int64_t sx,
                                 int64_t sy,
                                 int64_t w,
                                 int64_t h);

/// @brief Fill an inclusive horizontal span [x0..x1] at logical row @p y as a
///        height-1 logical rect (scale-aware; internal, defined in rt_drawing.c).
///        Scanline-fill primitives use this instead of rt_canvas_line so HiDPI
///        canvases don't render striped fills.
/// @param canvas_ptr Destination Canvas handle.
/// @param x0 Inclusive left endpoint.
/// @param x1 Inclusive right endpoint.
/// @param y Logical row coordinate.
/// @param color Packed span color.
void rt_canvas_fill_hspan(void *canvas_ptr, int64_t x0, int64_t x1, int64_t y, int64_t color);
