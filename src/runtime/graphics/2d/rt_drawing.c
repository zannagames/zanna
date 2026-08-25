//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_drawing.c
/// @file
/// @brief Implements clipped logical-pixel primitives, bitmap text, color
///        constructors, pixel blits, framebuffer capture, and image export.
// Purpose: Basic drawing primitives for the Canvas runtime. Includes line, box,
//   frame, disc, ring, plot, text rendering (normal and scaled), alpha-blended
//   shapes, pixel blitting (opaque and alpha), get_pixel, copy_rect, and
//   save_bmp/save_png.
//
// Key invariants:
//   - Drawing entry points validate the Canvas/native-window handles and become
//     no-ops when unavailable.
//   - Primitive colors are converted to opaque RGB. Alpha-specific operations
//     receive a separate alpha value; Pixels blits either copy or composite
//     their stored RGBA texels.
//   - Coordinate origin is top-left; x increases right, y increases down.
//   - Logical clipping occurs before backend int32 conversion. HiDPI blits and
//     filled spans expand logical pixels to their physical framebuffer blocks.
//
// Ownership/Lifetime:
//   - Canvas, Pixels, path, and text parameters are borrowed.
//   - CopyRect returns a newly owned Pixels object. Save helpers own and release
//     their temporary snapshot.
//
// Links: src/runtime/graphics/common/rt_graphics_internal.h,
//        src/runtime/graphics/common/rt_graphics.h (public API),
//        rt_font.h (glyph data), rt_pixels.h (Pixels buffer)
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_internal.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Check whether `ZANNA_TRACE_CANVAS_BOX` is set, with one-shot caching.
/// @details The env-var lookup happens once on first call and is cached
///          in a static so the trace check on every `Canvas.Box()` is
///          a single int compare. Used by debug builds to log the
///          first 32 box draws — useful when reproducing layout bugs.
/// @return `1` when the environment variable was present during the first
///         query; otherwise `0`.
static int rt_trace_canvas_box_enabled(void) {
    static int cached = -1;
    if (cached == -1)
        cached = getenv("ZANNA_TRACE_CANVAS_BOX") ? 1 : 0;
    return cached;
}

/// @brief Decode the next UTF-8 codepoint from `str` starting at `*index`.
/// @details Implements the standard 1/2/3/4-byte UTF-8 walk:
///          - `0xxxxxxx`            → ASCII (1 byte).
///          - `110xxxxx 10xxxxxx`   → U+0080..U+07FF (2 bytes).
///          - `1110xxxx 10xxxxxx ×2` → U+0800..U+FFFF (3 bytes).
///          - `11110xxx 10xxxxxx ×3` → U+10000..U+10FFFF (4 bytes).
///          For each multi-byte form, validates the continuation bytes
///          (`10xxxxxx`) and rejects:
///          - Overlong encodings (e.g. 2-byte sequence < U+0080).
///          - Surrogate halves (U+D800..U+DFFF) — never legal in UTF-8.
///          - Out-of-range scalars (> U+10FFFF).
///          Substitutes `?` (U+003F) for any malformed sequence so the
///          renderer never blows up on bad input. Advances `*index`
///          by the consumed byte count and returns 0 at EOF.
/// @param str Borrowed UTF-8 byte buffer.
/// @param byte_len Number of readable bytes in @p str.
/// @param index Required in/out byte offset.
/// @param codepoint_out Required output for the decoded scalar or `'?'`.
/// @return `1` when one input unit was consumed; `0` for invalid pointers or EOF.
static int rt_canvas_next_codepoint(const char *str,
                                    size_t byte_len,
                                    size_t *index,
                                    int *codepoint_out) {
    if (!str || !index || !codepoint_out || *index >= byte_len)
        return 0;

    size_t i = *index;
    unsigned char c0 = (unsigned char)str[i];
    uint32_t cp = '?';
    size_t advance = 1;

    if (c0 < 0x80) {
        cp = c0;
    } else if ((c0 & 0xE0u) == 0xC0u && i + 1 < byte_len) {
        unsigned char c1 = (unsigned char)str[i + 1];
        if ((c1 & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c0 & 0x1Fu) << 6) | (uint32_t)(c1 & 0x3Fu);
            advance = 2;
            if (cp < 0x80u)
                cp = '?';
        }
    } else if ((c0 & 0xF0u) == 0xE0u && i + 2 < byte_len) {
        unsigned char c1 = (unsigned char)str[i + 1];
        unsigned char c2 = (unsigned char)str[i + 2];
        if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c0 & 0x0Fu) << 12) | ((uint32_t)(c1 & 0x3Fu) << 6) |
                 (uint32_t)(c2 & 0x3Fu);
            advance = 3;
            if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))
                cp = '?';
        }
    } else if ((c0 & 0xF8u) == 0xF0u && i + 3 < byte_len) {
        unsigned char c1 = (unsigned char)str[i + 1];
        unsigned char c2 = (unsigned char)str[i + 2];
        unsigned char c3 = (unsigned char)str[i + 3];
        if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u && (c3 & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c0 & 0x07u) << 18) | ((uint32_t)(c1 & 0x3Fu) << 12) |
                 ((uint32_t)(c2 & 0x3Fu) << 6) | (uint32_t)(c3 & 0x3Fu);
            advance = 4;
            if (cp < 0x10000u || cp > 0x10FFFFu)
                cp = '?';
        }
    }

    *index = i + advance;
    *codepoint_out = (int)cp;
    return 1;
}

/// @brief Validate a runtime text string and expose its counted byte range.
/// @param text Borrowed runtime string.
/// @param bytes_out Required output receiving the borrowed byte pointer.
/// @param byte_len_out Required output receiving the safe `size_t` length.
/// @return Non-zero for a valid string representation, including an empty one.
static int8_t rt_canvas_text_bytes(rt_string text, const char **bytes_out, size_t *byte_len_out) {
    if (bytes_out)
        *bytes_out = NULL;
    if (byte_len_out)
        *byte_len_out = 0;
    if (!text || !bytes_out || !byte_len_out)
        return 0;
    const char *bytes = rt_string_cstr(text);
    int64_t raw_len = rt_str_len(text);
    if (!bytes || raw_len < 0 || (uint64_t)raw_len > SIZE_MAX)
        return 0;
    *bytes_out = bytes;
    *byte_len_out = (size_t)raw_len;
    return 1;
}

/// @brief Measure rendered text width by counting UTF-8 codepoints (not bytes).
/// @details The 8×8 bitmap font is monospace, so width is simply
///          `codepoints * 8 * scale`. Counting *codepoints* (not raw
///          bytes) ensures non-ASCII glyphs and ASCII glyphs both
///          contribute exactly one cell — `"héllo"` measures the same
///          as `"hello"` regardless of UTF-8 byte length.
/// @param text Borrowed runtime string.
/// @param scale Positive integer glyph scale.
/// @return Saturating logical width, or `0` for invalid text/scale.
static int64_t rt_canvas_text_codepoint_width(rt_string text, int64_t scale) {
    if (!text || scale < 1)
        return 0;

    const char *str = NULL;
    size_t byte_len = 0;
    if (!rt_canvas_text_bytes(text, &str, &byte_len) || byte_len == 0)
        return 0;
    size_t index = 0;
    int64_t count = 0;
    int codepoint = 0;
    while (rt_canvas_next_codepoint(str, byte_len, &index, &codepoint))
        count++;
    return rtg_mul_sat64(rtg_mul_sat64(count, 8), scale);
}

/// @brief Round a long double to the nearest int64, saturating at INT64_MIN/MAX instead of
/// overflowing.
/// @param value Finite value to convert.
/// @return Nearest int64 with ties away from zero and endpoint saturation.
static int64_t rt_canvas_round_ld_to_i64_sat(long double value) {
    if (isnan(value))
        return 0;
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)(value >= 0.0L ? floorl(value + 0.5L) : ceill(value - 0.5L));
}

/// @brief Floor a long double to int64, saturating at INT64_MIN/MAX instead of overflowing.
/// @param value Finite value to convert.
/// @return Mathematical floor saturated to the int64 range.
static int64_t rt_canvas_floor_ld_to_i64_sat(long double value) {
    if (isnan(value))
        return 0;
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)floorl(value);
}

/// @brief Ceil a long double to int64, saturating at INT64_MIN/MAX instead of overflowing.
/// @param value Finite value to convert.
/// @return Mathematical ceiling saturated to the int64 range.
static int64_t rt_canvas_ceil_ld_to_i64_sat(long double value) {
    if (isnan(value))
        return 0;
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)ceill(value);
}

/// @brief Convert two's-complement unsigned bits to int64 without an
///        implementation-defined out-of-range cast.
static int64_t rt_canvas_i64_from_bits(uint64_t bits) {
    if (bits <= (uint64_t)INT64_MAX)
        return (int64_t)bits;
    return INT64_MIN + (int64_t)(bits - (UINT64_C(1) << 63u));
}

/// @brief Compute round(value * numerator / denominator) for a unit ratio.
/// @details A bitwise quotient/remainder recurrence avoids compiler-specific
///          128-bit integers and platform-dependent floating-point precision.
static uint64_t rt_canvas_scale_by_ratio_u64(uint64_t value,
                                             uint64_t numerator,
                                             uint64_t denominator) {
    if (value == 0 || numerator == 0 || denominator == 0)
        return 0;
    if (numerator >= denominator)
        return value;

    uint64_t quotient = 0;
    uint64_t remainder = 0;
    for (int bit = 63; bit >= 0; bit--) {
        uint64_t carry = 0;
        if (remainder >= denominator - remainder) {
            remainder -= denominator - remainder;
            carry = 1;
        } else {
            remainder += remainder;
        }
        quotient = quotient * 2u + carry;

        if (((value >> (unsigned)bit) & 1u) != 0u) {
            if (remainder >= denominator - numerator) {
                remainder -= denominator - numerator;
                quotient++;
            } else {
                remainder += numerator;
            }
        }
    }

    if (remainder >= denominator / 2u + denominator % 2u)
        quotient++;
    return quotient;
}

/// @brief Interpolate one signed coordinate at an exact segment ratio.
static int64_t rt_canvas_interpolate_i64(int64_t start,
                                         int64_t end,
                                         uint64_t numerator,
                                         uint64_t denominator) {
    if (numerator == 0 || start == end)
        return start;
    if (numerator >= denominator)
        return end;
    uint64_t full_magnitude =
        start >= end ? (uint64_t)start - (uint64_t)end : (uint64_t)end - (uint64_t)start;
    uint64_t magnitude = rt_canvas_scale_by_ratio_u64(full_magnitude, numerator, denominator);
    uint64_t bits = end >= start ? (uint64_t)start + magnitude : (uint64_t)start - magnitude;
    return rt_canvas_i64_from_bits(bits);
}

enum {
    RT_CANVAS_CLIP_LEFT = 1u,
    RT_CANVAS_CLIP_RIGHT = 2u,
    RT_CANVAS_CLIP_TOP = 4u,
    RT_CANVAS_CLIP_BOTTOM = 8u
};

/// @brief Return the Cohen-Sutherland outcode for one signed point.
static uint8_t rt_canvas_clip_outcode(
    int64_t min_x, int64_t min_y, int64_t max_x, int64_t max_y, int64_t x, int64_t y) {
    uint8_t code = 0;
    if (x < min_x)
        code |= RT_CANVAS_CLIP_LEFT;
    else if (x > max_x)
        code |= RT_CANVAS_CLIP_RIGHT;
    if (y < min_y)
        code |= RT_CANVAS_CLIP_TOP;
    else if (y > max_y)
        code |= RT_CANVAS_CLIP_BOTTOM;
    return code;
}

/// @brief Clip a line segment to the canvas's logical clip rect.
/// @details Cohen-Sutherland iteration computes boundary crossings with exact
///          unsigned ratio arithmetic. This keeps int64-extreme endpoints
///          deterministic on Windows (where long double is double precision)
///          and on platforms with extended-precision long double.
/// @param canvas Canvas whose clip rect provides the bounds.
/// @param x1 In/out line-start X, replaced with the clipped value on success.
/// @param y1 In/out line-start Y, replaced with the clipped value on success.
/// @param x2 In/out line-end X, replaced with the clipped value on success.
/// @param y2 In/out line-end Y, replaced with the clipped value on success.
/// @return 1 if any portion of the line is visible (endpoints updated), 0 if
///         fully outside the clip rect or if endpoints don't fit int32.
static int8_t rt_canvas_clip_line_to_logical(
    rt_canvas *canvas, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2) {
    if (!x1 || !y1 || !x2 || !y2)
        return 0;

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return 0;

    int64_t min_x = clip_x;
    int64_t min_y = clip_y;
    int64_t max_x = rtg_add_sat64(clip_x, clip_w - 1);
    int64_t max_y = rtg_add_sat64(clip_y, clip_h - 1);
    uint8_t code1 = rt_canvas_clip_outcode(min_x, min_y, max_x, max_y, *x1, *y1);
    uint8_t code2 = rt_canvas_clip_outcode(min_x, min_y, max_x, max_y, *x2, *y2);

    for (int iteration = 0; iteration < 8; iteration++) {
        if ((code1 | code2) == 0)
            return rtg_i64_fits_i32(*x1) && rtg_i64_fits_i32(*y1) && rtg_i64_fits_i32(*x2) &&
                   rtg_i64_fits_i32(*y2);
        if ((code1 & code2) != 0)
            return 0;

        uint8_t code = code1 != 0 ? code1 : code2;
        int64_t next_x = 0;
        int64_t next_y = 0;
        if ((code & (RT_CANVAS_CLIP_TOP | RT_CANVAS_CLIP_BOTTOM)) != 0) {
            next_y = (code & RT_CANVAS_CLIP_TOP) != 0 ? min_y : max_y;
            uint64_t denominator =
                *y1 >= *y2 ? (uint64_t)*y1 - (uint64_t)*y2 : (uint64_t)*y2 - (uint64_t)*y1;
            uint64_t numerator =
                next_y >= *y1 ? (uint64_t)next_y - (uint64_t)*y1 : (uint64_t)*y1 - (uint64_t)next_y;
            if (denominator == 0 || numerator > denominator)
                return 0;
            next_x = rt_canvas_interpolate_i64(*x1, *x2, numerator, denominator);
        } else {
            next_x = (code & RT_CANVAS_CLIP_LEFT) != 0 ? min_x : max_x;
            uint64_t denominator =
                *x1 >= *x2 ? (uint64_t)*x1 - (uint64_t)*x2 : (uint64_t)*x2 - (uint64_t)*x1;
            uint64_t numerator =
                next_x >= *x1 ? (uint64_t)next_x - (uint64_t)*x1 : (uint64_t)*x1 - (uint64_t)next_x;
            if (denominator == 0 || numerator > denominator)
                return 0;
            next_y = rt_canvas_interpolate_i64(*y1, *y2, numerator, denominator);
        }

        if (code == code1) {
            *x1 = next_x;
            *y1 = next_y;
            code1 = rt_canvas_clip_outcode(min_x, min_y, max_x, max_y, *x1, *y1);
        } else {
            *x2 = next_x;
            *y2 = next_y;
            code2 = rt_canvas_clip_outcode(min_x, min_y, max_x, max_y, *x2, *y2);
        }
    }
    return 0;
}

/// @brief Compute the last inclusive pixel of a rect span: start + max(length-1, 0), saturating.
/// @param start First coordinate.
/// @param length Span length; values at most one return @p start.
/// @return Saturating inclusive final coordinate.
static int64_t rt_canvas_rect_last(int64_t start, int64_t length) {
    if (length <= 1)
        return start;
    return rtg_add_sat64(start, length - 1);
}

/// @brief Convert a Zanna packed color to the opaque 24-bit RGB value expected by ZannaGFX.
/// @param color Runtime RGB, tagged ARGB, or raw RGBA color.
/// @return Opaque backend `0xRRGGBB` value with alpha discarded.
static vgfx_color_t rt_canvas_color_to_vgfx_rgb(int64_t color) {
    return (vgfx_color_t)((rt_pixels_color_to_rgba(color) >> 8) & 0x00FFFFFFu);
}

/// @brief Draw a line between two points on the canvas.
/// @details Clips in logical coordinates, rejects endpoints that cannot fit the
///          backend int32 API after clipping, and draws an opaque stroke.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x1 Logical start X.
/// @param y1 Logical start Y.
/// @param x2 Logical end X.
/// @param y2 Logical end Y.
/// @param color Packed stroke color; effective alpha is ignored.
void rt_canvas_line(
    void *canvas_ptr, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color) {
    if (!canvas_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        rt_canvas_resync_window_state(canvas);
        if (!rt_canvas_clip_line_to_logical(canvas, &x1, &y1, &x2, &y2))
            return;
        vgfx_line(canvas->gfx_win,
                  (int32_t)x1,
                  (int32_t)y1,
                  (int32_t)x2,
                  (int32_t)y2,
                  rt_canvas_color_to_vgfx_rgb(color));
    }
}

/// @brief Draw a filled rectangle on the canvas.
/// @details Intersects the rectangle with the current logical clip and backend
///          coordinate limits. Optional environment tracing logs the first 32
///          calls before validation.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge.
/// @param y Logical top edge.
/// @param w Width; nonpositive/clipped-empty rectangles draw nothing.
/// @param h Height; nonpositive/clipped-empty rectangles draw nothing.
/// @param color Packed fill color; effective alpha is ignored.
void rt_canvas_box(void *canvas_ptr, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    static int trace_count = 0;
    if (!canvas_ptr)
        return;

    if (rt_trace_canvas_box_enabled() && trace_count < 32) {
        fprintf(stderr,
                "[rt_canvas_box] #%d x=%lld y=%lld w=%lld h=%lld color=%#llx\n",
                trace_count,
                (long long)x,
                (long long)y,
                (long long)w,
                (long long)h,
                (unsigned long long)color);
        ++trace_count;
    }

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        rt_canvas_resync_window_state(canvas);
        if (!rt_canvas_clip_intersect_logical(canvas, &x, &y, &w, &h))
            return;
        vgfx_fill_rect(canvas->gfx_win,
                       (int32_t)x,
                       (int32_t)y,
                       (int32_t)w,
                       (int32_t)h,
                       rt_canvas_color_to_vgfx_rgb(color));
    }
}

/// @brief Draw an unfilled rectangle (outline) on the canvas.
/// @details Builds four clipped inclusive-edge lines. Nonpositive dimensions
///          draw nothing.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge.
/// @param y Logical top edge.
/// @param w Outline width.
/// @param h Outline height.
/// @param color Packed stroke color; effective alpha is ignored.
void rt_canvas_frame(void *canvas_ptr, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    if (!canvas_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        if (w <= 0 || h <= 0)
            return;
        int64_t x1 = rt_canvas_rect_last(x, w);
        int64_t y1 = rt_canvas_rect_last(y, h);
        rt_canvas_line(canvas_ptr, x, y, x1, y, color);
        rt_canvas_line(canvas_ptr, x, y1, x1, y1, color);
        rt_canvas_line(canvas_ptr, x, y, x, y1, color);
        rt_canvas_line(canvas_ptr, x1, y, x1, y1, color);
    }
}

/// @brief Rasterize a filled circle only across visible clip rows.
/// @details Computes each scanline extent with long-double square roots and
///          saturating conversion, then intersects the run with the clip.
/// @param canvas Borrowed live Canvas implementation.
/// @param cx Logical center X.
/// @param cy Logical center Y.
/// @param radius Nonnegative logical radius.
/// @param color Backend RGB color.
/// @param clip_x Clip left edge.
/// @param clip_y Clip top edge.
/// @param clip_w Positive clip width.
/// @param clip_h Positive clip height.
static void rt_canvas_disc_clipped_safe(rt_canvas *canvas,
                                        int64_t cx,
                                        int64_t cy,
                                        int64_t radius,
                                        vgfx_color_t color,
                                        int64_t clip_x,
                                        int64_t clip_y,
                                        int64_t clip_w,
                                        int64_t clip_h);
/// @brief Rasterize a connected circle outline only across visible clip rows.
/// @details Joins each row's rounded left/right intersections to those from
///          the preceding row so steep arc sections do not contain gaps.
/// @param canvas Borrowed live Canvas implementation.
/// @param cx Logical center X.
/// @param cy Logical center Y.
/// @param radius Nonnegative logical radius.
/// @param color Backend RGB color.
/// @param clip_x Clip left edge.
/// @param clip_y Clip top edge.
/// @param clip_w Positive clip width.
/// @param clip_h Positive clip height.
static void rt_canvas_ring_clipped_safe(rt_canvas *canvas,
                                        int64_t cx,
                                        int64_t cy,
                                        int64_t radius,
                                        vgfx_color_t color,
                                        int64_t clip_x,
                                        int64_t clip_y,
                                        int64_t clip_w,
                                        int64_t clip_h);

/// @brief Draw a filled circle on the canvas.
/// @details Negative radius is ignored; zero draws one pixel. Normal int32
///          circles use the backend fast path, while huge/out-of-range circles
///          use clipped scanline rasterization bounded by the viewport.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param cx Logical center X.
/// @param cy Logical center Y.
/// @param radius Nonnegative radius in logical pixels.
/// @param color Packed fill color; effective alpha is ignored.
void rt_canvas_disc(void *canvas_ptr, int64_t cx, int64_t cy, int64_t radius, int64_t color) {
    if (!canvas_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        rt_canvas_resync_window_state(canvas);
        if (radius < 0)
            return;
        vgfx_color_t rgb = rt_canvas_color_to_vgfx_rgb(color);
        int64_t clip_x = 0;
        int64_t clip_y = 0;
        int64_t clip_w = 0;
        int64_t clip_h = 0;
        int8_t have_clip =
            rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h);
        /* For a radius that dwarfs the viewport, the clipped scanline path visits
         * only the visible rows (O(clip_h)); vgfx_fill_circle would spin the full
         * O(radius) octant even when almost everything is off-screen. */
        if (have_clip && radius > clip_w + clip_h) {
            rt_canvas_disc_clipped_safe(
                canvas, cx, cy, radius, rgb, clip_x, clip_y, clip_w, clip_h);
            return;
        }
        if (rtg_i64_fits_i32(cx) && rtg_i64_fits_i32(cy) && rtg_i64_fits_i32(radius)) {
            vgfx_fill_circle(canvas->gfx_win, (int32_t)cx, (int32_t)cy, (int32_t)radius, rgb);
            return;
        }
        if (!have_clip)
            return;
        rt_canvas_disc_clipped_safe(canvas, cx, cy, radius, rgb, clip_x, clip_y, clip_w, clip_h);
    }
}

/// @brief Draw an unfilled circle (outline) on the canvas.
/// @details Negative radius is ignored; zero draws one pixel. Huge/out-of-range
///          circles use a clip-bounded connected scanline outline.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param cx Logical center X.
/// @param cy Logical center Y.
/// @param radius Nonnegative radius in logical pixels.
/// @param color Packed stroke color; effective alpha is ignored.
void rt_canvas_ring(void *canvas_ptr, int64_t cx, int64_t cy, int64_t radius, int64_t color) {
    if (!canvas_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        rt_canvas_resync_window_state(canvas);
        if (radius < 0)
            return;
        vgfx_color_t rgb = rt_canvas_color_to_vgfx_rgb(color);
        int64_t clip_x = 0;
        int64_t clip_y = 0;
        int64_t clip_w = 0;
        int64_t clip_h = 0;
        int8_t have_clip =
            rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h);
        /* Huge radius vs a small viewport: the clipped scanline path visits only the
         * visible rows rather than vgfx_circle's full O(radius) octant. */
        if (have_clip && radius > clip_w + clip_h) {
            rt_canvas_ring_clipped_safe(
                canvas, cx, cy, radius, rgb, clip_x, clip_y, clip_w, clip_h);
            return;
        }
        if (rtg_i64_fits_i32(cx) && rtg_i64_fits_i32(cy) && rtg_i64_fits_i32(radius)) {
            vgfx_circle(canvas->gfx_win, (int32_t)cx, (int32_t)cy, (int32_t)radius, rgb);
            return;
        }
        if (!have_clip)
            return;
        rt_canvas_ring_clipped_safe(canvas, cx, cy, radius, rgb, clip_x, clip_y, clip_w, clip_h);
    }
}

/// @brief Draw a single pixel at the given coordinates.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical X coordinate.
/// @param y Logical Y coordinate.
/// @param color Packed color; effective alpha is ignored.
void rt_canvas_plot(void *canvas_ptr, int64_t x, int64_t y, int64_t color) {
    if (!canvas_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        rt_canvas_resync_window_state(canvas);
        int64_t w = 1;
        int64_t h = 1;
        if (!rt_canvas_clip_intersect_logical(canvas, &x, &y, &w, &h))
            return;
        vgfx_pset(canvas->gfx_win, (int32_t)x, (int32_t)y, rt_canvas_color_to_vgfx_rgb(color));
    }
}

// Color constants — packed 0x00RRGGBB
/// @brief Return the predefined red color constant.
/// @return Plain RGB `0xFF0000`.
int64_t rt_color_red(void) {
    return 0xFF0000;
}

/// @brief Return the predefined green color constant.
/// @return Plain RGB `0x00FF00`.
int64_t rt_color_green(void) {
    return 0x00FF00;
}

/// @brief Return the predefined blue color constant.
/// @return Plain RGB `0x0000FF`.
int64_t rt_color_blue(void) {
    return 0x0000FF;
}

/// @brief Return the predefined white color constant.
/// @return Plain RGB `0xFFFFFF`.
int64_t rt_color_white(void) {
    return 0xFFFFFF;
}

/// @brief Return the predefined black color constant.
/// @return Plain RGB `0x000000`.
int64_t rt_color_black(void) {
    return 0x000000;
}

/// @brief Return the predefined yellow color constant.
/// @return Plain RGB `0xFFFF00`.
int64_t rt_color_yellow(void) {
    return 0xFFFF00;
}

/// @brief Return the predefined cyan color constant.
/// @return Plain RGB `0x00FFFF`.
int64_t rt_color_cyan(void) {
    return 0x00FFFF;
}

/// @brief Return the predefined magenta color constant.
/// @return Plain RGB `0xFF00FF`.
int64_t rt_color_magenta(void) {
    return 0xFF00FF;
}

/// @brief Return the predefined gray color constant.
/// @return Plain RGB `0x808080`.
int64_t rt_color_gray(void) {
    return 0x808080;
}

/// @brief Return the predefined orange color constant.
/// @return Plain RGB `0xFFA500`.
int64_t rt_color_orange(void) {
    return 0xFFA500;
}

/// @brief Construct a color from red, green, blue components (0-255).
/// @param r Red channel, clamped to 0..255.
/// @param g Green channel, clamped to 0..255.
/// @param b Blue channel, clamped to 0..255.
/// @return Plain implicit-alpha `0xRRGGBB` color.
int64_t rt_color_rgb(int64_t r, int64_t g, int64_t b) {
    uint8_t r8 = (r < 0) ? 0 : (r > 255) ? 255 : (uint8_t)r;
    uint8_t g8 = (g < 0) ? 0 : (g > 255) ? 255 : (uint8_t)g;
    uint8_t b8 = (b < 0) ? 0 : (b > 255) ? 255 : (uint8_t)b;
    return (int64_t)vgfx_rgb(r8, g8, b8);
}

/// @brief Construct a color from red, green, blue, alpha components (0-255).
/// @param r Red channel, clamped to 0..255.
/// @param g Green channel, clamped to 0..255.
/// @param b Blue channel, clamped to 0..255.
/// @param a Alpha channel, clamped to 0..255.
/// @return Tagged explicit-alpha runtime `0xAARRGGBB` color.
int64_t rt_color_rgba(int64_t r, int64_t g, int64_t b, int64_t a) {
    uint8_t r8 = (r < 0) ? 0 : (r > 255) ? 255 : (uint8_t)r;
    uint8_t g8 = (g < 0) ? 0 : (g > 255) ? 255 : (uint8_t)g;
    uint8_t b8 = (b < 0) ? 0 : (b > 255) ? 255 : (uint8_t)b;
    uint8_t a8 = (a < 0) ? 0 : (a > 255) ? 255 : (uint8_t)a;
    int64_t packed =
        (int64_t)(((uint32_t)a8 << 24) | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | (uint32_t)b8);
    return packed | RT_COLOR_EXPLICIT_ALPHA_FLAG;
}

/// @brief Test whether (x, y) is inside the clip rect [clip_x, clip_x+clip_w) × [clip_y,
/// clip_y+clip_h)
///        using saturating int64 math, with a bonus int32-fits check for the eventual vgfx call.
/// @details Used by the per-pixel plot/fill helpers below. Saturating addition
///          ensures clip_x + clip_w doesn't wrap on extreme inputs.
/// @param x Point X.
/// @param y Point Y.
/// @param clip_x Clip left edge.
/// @param clip_y Clip top edge.
/// @param clip_w Positive clip width.
/// @param clip_h Positive clip height.
/// @return `1` if the point is inside the clip and fits int32; otherwise `0`.
static int8_t rt_canvas_point_in_clip_i64(
    int64_t x, int64_t y, int64_t clip_x, int64_t clip_y, int64_t clip_w, int64_t clip_h) {
    if (clip_w <= 0 || clip_h <= 0)
        return 0;
    if (x < clip_x || y < clip_y)
        return 0;
    if (x >= rtg_add_sat64(clip_x, clip_w) || y >= rtg_add_sat64(clip_y, clip_h))
        return 0;
    return rtg_i64_fits_i32(x) && rtg_i64_fits_i32(y);
}

/// @brief Plot one pixel at (x, y) iff it falls inside the clip rect.
/// @details Used by line/disc/ring/text rasterizers that step pixel-by-pixel
///          and want clip-correct behavior without paying for a separate
///          ZannaGFX clip-set per pixel. NULL canvas / NULL gfx_win are no-ops.
/// @param canvas Borrowed Canvas implementation.
/// @param x Logical pixel X.
/// @param y Logical pixel Y.
/// @param color Backend RGB color.
/// @param clip_x Clip left edge.
/// @param clip_y Clip top edge.
/// @param clip_w Clip width.
/// @param clip_h Clip height.
static void rt_canvas_pset_clipped(rt_canvas *canvas,
                                   int64_t x,
                                   int64_t y,
                                   vgfx_color_t color,
                                   int64_t clip_x,
                                   int64_t clip_y,
                                   int64_t clip_w,
                                   int64_t clip_h) {
    if (!canvas || !canvas->gfx_win ||
        !rt_canvas_point_in_clip_i64(x, y, clip_x, clip_y, clip_w, clip_h))
        return;
    vgfx_pset(canvas->gfx_win, (int32_t)x, (int32_t)y, color);
}

/// @brief Fill the intersection of the (x, y, w, h) rect with the clip rect.
/// @details Computes [x0, x1) × [y0, y1) as the intersection in saturating
///          int64 math, then verifies each side fits in int32 before issuing
///          the vgfx_fill_rect call. Empty intersections (x1 <= x0) are no-ops.
///          Used by box/frame primitives that may straddle the clip boundary.
/// @param canvas Borrowed Canvas implementation.
/// @param x Rectangle left edge.
/// @param y Rectangle top edge.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param color Backend RGB color.
/// @param clip_x Clip left edge.
/// @param clip_y Clip top edge.
/// @param clip_w Clip width.
/// @param clip_h Clip height.
static void rt_canvas_fill_rect_clipped(rt_canvas *canvas,
                                        int64_t x,
                                        int64_t y,
                                        int64_t w,
                                        int64_t h,
                                        vgfx_color_t color,
                                        int64_t clip_x,
                                        int64_t clip_y,
                                        int64_t clip_w,
                                        int64_t clip_h) {
    if (!canvas || !canvas->gfx_win || w <= 0 || h <= 0 || clip_w <= 0 || clip_h <= 0)
        return;

    int64_t x0 = rtg_max64(x, clip_x);
    int64_t y0 = rtg_max64(y, clip_y);
    int64_t x1 = rtg_min64(rtg_add_sat64(x, w), rtg_add_sat64(clip_x, clip_w));
    int64_t y1 = rtg_min64(rtg_add_sat64(y, h), rtg_add_sat64(clip_y, clip_h));
    if (x1 <= x0 || y1 <= y0)
        return;
    if (!rtg_i64_fits_i32(x0) || !rtg_i64_fits_i32(y0) || !rtg_i64_fits_i32(x1 - x0) ||
        !rtg_i64_fits_i32(y1 - y0))
        return;
    vgfx_fill_rect(
        canvas->gfx_win, (int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0), color);
}

/// @brief Compute the inclusive width of a horizontal span with saturation.
/// @param x0 First inclusive coordinate.
/// @param x1 Last inclusive coordinate.
/// @return `x1 - x0 + 1` with int64 saturation, or `0` for reversed spans.
static int64_t rt_canvas_span_width_sat(int64_t x0, int64_t x1) {
    if (x1 < x0)
        return 0;
    uint64_t difference = (uint64_t)x1 - (uint64_t)x0;
    if (difference >= (uint64_t)INT64_MAX)
        return INT64_MAX;
    return (int64_t)difference + 1;
}

/// @brief Fill an inclusive horizontal span [x0..x1] at logical row @p y.
/// @details Internal helper for scanline-fill primitives (triangle/polygon/ellipse/
///   rounded-box/thick-line). Draws the run as a height-1 *logical* rectangle via
///   the scale-aware rect path, so on a HiDPI canvas (coord_scale > 1) the row is
///   thickened to fill the corresponding physical rows. Using rt_canvas_line for
///   these spans instead leaves gaps between physical rows (vgfx_line scales only
///   its endpoints, not the stroke width), producing striped fills.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x0 First inclusive logical X; order may be reversed.
/// @param x1 Last inclusive logical X.
/// @param y Logical row.
/// @param color Packed fill color; effective alpha is ignored.
void rt_canvas_fill_hspan(void *canvas_ptr, int64_t x0, int64_t x1, int64_t y, int64_t color) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    if (x1 < x0) {
        int64_t tmp = x0;
        x0 = x1;
        x1 = tmp;
    }
    rt_canvas_resync_window_state(canvas);
    int64_t clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t span = rt_canvas_span_width_sat(x0, x1);
    rt_canvas_fill_rect_clipped(
        canvas, x0, y, span, 1, rt_canvas_color_to_vgfx_rgb(color), clip_x, clip_y, clip_w, clip_h);
}

/// @copydetails rt_canvas_disc_clipped_safe
static void rt_canvas_disc_clipped_safe(rt_canvas *canvas,
                                        int64_t cx,
                                        int64_t cy,
                                        int64_t radius,
                                        vgfx_color_t color,
                                        int64_t clip_x,
                                        int64_t clip_y,
                                        int64_t clip_w,
                                        int64_t clip_h) {
    if (!canvas || radius < 0 || clip_w <= 0 || clip_h <= 0)
        return;
    if (radius == 0) {
        rt_canvas_pset_clipped(canvas, cx, cy, color, clip_x, clip_y, clip_w, clip_h);
        return;
    }

    int64_t y0 = rtg_sub_nonneg_sat64(cy, radius);
    int64_t y1 = rtg_add_sat64(cy, radius);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h) - 1;
    if (y0 < clip_y)
        y0 = clip_y;
    if (y1 > clip_y1)
        y1 = clip_y1;
    if (y1 < y0)
        return;

    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w) - 1;
    long double r2 = (long double)radius * (long double)radius;
    for (int64_t py = y0; py <= y1; py++) {
        long double dy = (long double)py - (long double)cy;
        long double rem = r2 - dy * dy;
        if (rem < 0.0L)
            continue;
        long double dx = sqrtl(rem);
        int64_t x0 = rt_canvas_floor_ld_to_i64_sat((long double)cx - dx);
        int64_t x1 = rt_canvas_ceil_ld_to_i64_sat((long double)cx + dx);
        if (x0 < clip_x)
            x0 = clip_x;
        if (x1 > clip_x1)
            x1 = clip_x1;
        int64_t span = rt_canvas_span_width_sat(x0, x1);
        if (span > 0)
            rt_canvas_fill_rect_clipped(
                canvas, x0, py, span, 1, color, clip_x, clip_y, clip_w, clip_h);
        if (py == INT64_MAX)
            break;
    }
}

/// @copydetails rt_canvas_ring_clipped_safe
static void rt_canvas_ring_clipped_safe(rt_canvas *canvas,
                                        int64_t cx,
                                        int64_t cy,
                                        int64_t radius,
                                        vgfx_color_t color,
                                        int64_t clip_x,
                                        int64_t clip_y,
                                        int64_t clip_w,
                                        int64_t clip_h) {
    if (!canvas || radius < 0 || clip_w <= 0 || clip_h <= 0)
        return;
    if (radius == 0) {
        rt_canvas_pset_clipped(canvas, cx, cy, color, clip_x, clip_y, clip_w, clip_h);
        return;
    }

    int64_t y0 = rtg_sub_nonneg_sat64(cy, radius);
    int64_t y1 = rtg_add_sat64(cy, radius);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h) - 1;
    if (y0 < clip_y)
        y0 = clip_y;
    if (y1 > clip_y1)
        y1 = clip_y1;
    if (y1 < y0)
        return;

    long double r2 = (long double)radius * (long double)radius;
    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w) - 1;
    int64_t prev_left = 0, prev_right = 0;
    int8_t have_prev = 0;
    for (int64_t py = y0; py <= y1; py++) {
        long double dy = (long double)py - (long double)cy;
        long double rem = r2 - dy * dy;
        if (rem < 0.0L) {
            have_prev = 0;
            continue;
        }
        long double dx = sqrtl(rem);
        int64_t left = rt_canvas_round_ld_to_i64_sat((long double)cx - dx);
        int64_t right = rt_canvas_round_ld_to_i64_sat((long double)cx + dx);
        /* Where the circle is near-horizontal (top/bottom), dx jumps by many pixels
         * between adjacent rows. Plotting only the two endpoints leaves gaps, so
         * fill the horizontal run on each arc from this row's x toward the previous
         * row's x — keeping the outline connected (Bresenham-circle behaviour). */
        if (have_prev) {
            int64_t l_lo = left < prev_left ? left : prev_left;
            int64_t l_hi = left < prev_left ? prev_left : left;
            if (l_lo < clip_x)
                l_lo = clip_x;
            if (l_hi > clip_x1)
                l_hi = clip_x1;
            for (int64_t x = l_lo; x <= l_hi; x++) {
                rt_canvas_pset_clipped(canvas, x, py, color, clip_x, clip_y, clip_w, clip_h);
                if (x == INT64_MAX)
                    break;
            }
            int64_t r_lo = right < prev_right ? right : prev_right;
            int64_t r_hi = right < prev_right ? prev_right : right;
            if (r_lo < clip_x)
                r_lo = clip_x;
            if (r_hi > clip_x1)
                r_hi = clip_x1;
            for (int64_t x = r_lo; x <= r_hi; x++) {
                rt_canvas_pset_clipped(canvas, x, py, color, clip_x, clip_y, clip_w, clip_h);
                if (x == INT64_MAX)
                    break;
            }
        } else {
            rt_canvas_pset_clipped(canvas, left, py, color, clip_x, clip_y, clip_w, clip_h);
            if (right != left)
                rt_canvas_pset_clipped(canvas, right, py, color, clip_x, clip_y, clip_w, clip_h);
        }
        prev_left = left;
        prev_right = right;
        have_prev = 1;
        if (py == INT64_MAX)
            break;
    }
}

//=============================================================================
// Text Rendering
//=============================================================================

/// @brief Draw text at the given position on the canvas.
/// @details Decodes UTF-8, substitutes `'?'` for malformed or unsupported
///          codepoints, and draws one clipped 8x8 monochrome glyph per
///          codepoint. Text advances horizontally with no newline handling.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge of the first glyph.
/// @param y Logical top edge of the glyph row.
/// @param text Borrowed UTF-8 runtime string.
/// @param color Packed foreground color; effective alpha is ignored.
void rt_canvas_text(void *canvas_ptr, int64_t x, int64_t y, rt_string text, int64_t color) {
    if (!canvas_ptr || !text)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    const char *str = NULL;
    size_t byte_len = 0;
    if (!rt_canvas_text_bytes(text, &str, &byte_len))
        return;

    int64_t cx = x;
    vgfx_color_t col = rt_canvas_color_to_vgfx_rgb(color);
    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h);
    if (x >= clip_x1 || y >= clip_y1 || rtg_add_sat64(y, 8) <= clip_y)
        return;
    size_t index = 0;
    int codepoint = 0;

    while (rt_canvas_next_codepoint(str, byte_len, &index, &codepoint)) {
        if (cx >= clip_x1)
            break;
        int glyph_cp = (codepoint >= 32 && codepoint <= 126) ? codepoint : '?';
        const uint8_t *glyph = rt_font_get_glyph(glyph_cp);

        if (rtg_add_sat64(cx, 7) >= clip_x) {
            // Draw 8x8 glyph
            for (int row = 0; row < 8; row++) {
                uint8_t bits = glyph[row];
                for (int col_idx = 0; col_idx < 8; col_idx++) {
                    if (bits & (0x80 >> col_idx)) {
                        rt_canvas_pset_clipped(canvas,
                                               rtg_add_sat64(cx, col_idx),
                                               rtg_add_sat64(y, row),
                                               col,
                                               clip_x,
                                               clip_y,
                                               clip_w,
                                               clip_h);
                    }
                }
            }
        }
        cx = rtg_add_sat64(cx, 8);
    }
}

/// @brief Draw text at (x, y) with foreground @p fg and explicit @p bg fill behind each glyph.
/// Useful for status bars and code editors where the background must be opaque.
/// @details Every one of each glyph's 64 pixels is written, choosing foreground
///          for set font bits and background otherwise. UTF-8 fallback behavior
///          matches rt_canvas_text().
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge of the first glyph.
/// @param y Logical top edge of the glyph row.
/// @param text Borrowed UTF-8 runtime string.
/// @param fg Packed foreground color.
/// @param bg Packed background color.
void rt_canvas_text_bg(
    void *canvas_ptr, int64_t x, int64_t y, rt_string text, int64_t fg, int64_t bg) {
    if (!canvas_ptr || !text)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    const char *str = NULL;
    size_t byte_len = 0;
    if (!rt_canvas_text_bytes(text, &str, &byte_len))
        return;

    int64_t cx = x;
    vgfx_color_t fg_col = rt_canvas_color_to_vgfx_rgb(fg);
    vgfx_color_t bg_col = rt_canvas_color_to_vgfx_rgb(bg);
    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h);
    if (x >= clip_x1 || y >= clip_y1 || rtg_add_sat64(y, 8) <= clip_y)
        return;
    size_t index = 0;
    int codepoint = 0;

    while (rt_canvas_next_codepoint(str, byte_len, &index, &codepoint)) {
        if (cx >= clip_x1)
            break;
        int glyph_cp = (codepoint >= 32 && codepoint <= 126) ? codepoint : '?';
        const uint8_t *glyph = rt_font_get_glyph(glyph_cp);

        if (rtg_add_sat64(cx, 7) >= clip_x) {
            rt_canvas_fill_rect_clipped(
                canvas, cx, y, 8, 8, bg_col, clip_x, clip_y, clip_w, clip_h);
            for (int row = 0; row < 8; row++) {
                uint8_t bits = glyph[row];
                for (int col_idx = 0; col_idx < 8; col_idx++) {
                    if (bits & (0x80 >> col_idx))
                        rt_canvas_pset_clipped(canvas,
                                               rtg_add_sat64(cx, col_idx),
                                               rtg_add_sat64(y, row),
                                               fg_col,
                                               clip_x,
                                               clip_y,
                                               clip_w,
                                               clip_h);
                }
            }
        }
        cx = rtg_add_sat64(cx, 8);
    }
}

/// @brief Return the rendered width of `text` in pixels at 1× scale.
/// @details Counts UTF-8 codepoints (so multibyte glyphs each take one
///          cell of the monospace 8×8 font) and multiplies by 8.
/// @param text Borrowed UTF-8 runtime string.
/// @return Saturating logical width, or `0` for null text.
int64_t rt_canvas_text_width(rt_string text) {
    return rt_canvas_text_codepoint_width(text, 1);
}

/// @brief Return the height of one line of text in pixels — always 8 for the built-in font.
/// @return Constant logical height `8`.
int64_t rt_canvas_text_height(void) {
    return 8;
}

//=============================================================================
// Scaled Text Rendering
//=============================================================================

/// @brief Draw text with each pixel of the 8×8 built-in font expanded into a `scale × scale` rect.
/// Useful for HiDPI/big-pixel UIs without loading a separate larger font.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge of the first scaled glyph.
/// @param y Logical top edge of the glyph row.
/// @param text Borrowed UTF-8 runtime string.
/// @param scale Positive integer expansion factor.
/// @param color Packed foreground color; effective alpha is ignored.
void rt_canvas_text_scaled(
    void *canvas_ptr, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t color) {
    if (!canvas_ptr || !text || scale < 1)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    const char *str = NULL;
    size_t byte_len = 0;
    if (!rt_canvas_text_bytes(text, &str, &byte_len))
        return;

    int64_t cx = x;
    vgfx_color_t col = rt_canvas_color_to_vgfx_rgb(color);
    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t cell_width = rtg_mul_sat64(8, scale);
    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h);
    if (x >= clip_x1 || y >= clip_y1 || rtg_add_sat64(y, cell_width) <= clip_y)
        return;
    size_t index = 0;
    int codepoint = 0;

    while (rt_canvas_next_codepoint(str, byte_len, &index, &codepoint)) {
        if (cx >= clip_x1)
            break;
        int glyph_cp = (codepoint >= 32 && codepoint <= 126) ? codepoint : '?';
        const uint8_t *glyph = rt_font_get_glyph(glyph_cp);

        if (rtg_add_sat64(cx, cell_width - 1) >= clip_x) {
            for (int row = 0; row < 8; row++) {
                uint8_t bits = glyph[row];
                for (int col_idx = 0; col_idx < 8; col_idx++) {
                    if (bits & (0x80 >> col_idx)) {
                        rt_canvas_fill_rect_clipped(
                            canvas,
                            rtg_add_sat64(cx, rtg_mul_sat64(col_idx, scale)),
                            rtg_add_sat64(y, rtg_mul_sat64(row, scale)),
                            scale,
                            scale,
                            col,
                            clip_x,
                            clip_y,
                            clip_w,
                            clip_h);
                    }
                }
            }
        }
        cx = rtg_add_sat64(cx, cell_width);
    }
}

/// @brief Like `_text_scaled` but fills the @p bg color behind each glyph (full per-pixel cell).
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge of the first scaled glyph.
/// @param y Logical top edge of the glyph row.
/// @param text Borrowed UTF-8 runtime string.
/// @param scale Positive integer expansion factor.
/// @param fg Packed foreground color.
/// @param bg Packed background color.
void rt_canvas_text_scaled_bg(
    void *canvas_ptr, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t fg, int64_t bg) {
    if (!canvas_ptr || !text || scale < 1)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    const char *str = NULL;
    size_t byte_len = 0;
    if (!rt_canvas_text_bytes(text, &str, &byte_len))
        return;

    int64_t cx = x;
    vgfx_color_t fg_col = rt_canvas_color_to_vgfx_rgb(fg);
    vgfx_color_t bg_col = rt_canvas_color_to_vgfx_rgb(bg);
    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t cell_width = rtg_mul_sat64(8, scale);
    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h);
    if (x >= clip_x1 || y >= clip_y1 || rtg_add_sat64(y, cell_width) <= clip_y)
        return;
    size_t index = 0;
    int codepoint = 0;

    while (rt_canvas_next_codepoint(str, byte_len, &index, &codepoint)) {
        if (cx >= clip_x1)
            break;
        int glyph_cp = (codepoint >= 32 && codepoint <= 126) ? codepoint : '?';
        const uint8_t *glyph = rt_font_get_glyph(glyph_cp);

        if (rtg_add_sat64(cx, cell_width - 1) >= clip_x) {
            rt_canvas_fill_rect_clipped(
                canvas, cx, y, cell_width, cell_width, bg_col, clip_x, clip_y, clip_w, clip_h);
            for (int row = 0; row < 8; row++) {
                uint8_t bits = glyph[row];
                for (int col_idx = 0; col_idx < 8; col_idx++) {
                    if (bits & (0x80 >> col_idx))
                        rt_canvas_fill_rect_clipped(
                            canvas,
                            rtg_add_sat64(cx, rtg_mul_sat64(col_idx, scale)),
                            rtg_add_sat64(y, rtg_mul_sat64(row, scale)),
                            scale,
                            scale,
                            fg_col,
                            clip_x,
                            clip_y,
                            clip_w,
                            clip_h);
                }
            }
        }
        cx = rtg_add_sat64(cx, cell_width);
    }
}

/// @brief Return the rendered width of `text` in pixels when drawn at the given integer scale.
/// @param text Borrowed UTF-8 runtime string.
/// @param scale Positive integer expansion factor.
/// @return Saturating logical width, or `0` for invalid text/scale.
int64_t rt_canvas_text_scaled_width(rt_string text, int64_t scale) {
    return rt_canvas_text_codepoint_width(text, scale);
}

//=============================================================================
// Centered / Right-Aligned Text Helpers
//=============================================================================

/// @brief Draw `text` horizontally centered in the canvas at row `y`.
/// @details Pre-measures via `text_width`, then offsets x so the
///          rendered glyphs sit symmetrically about the canvas
///          centerline.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param y Logical top edge of the glyph row.
/// @param text Borrowed UTF-8 runtime string.
/// @param color Packed foreground color.
void rt_canvas_text_centered(void *canvas_ptr, int64_t y, rt_string text, int64_t color) {
    if (!canvas_ptr || !text)
        return;
    int64_t w = rt_canvas_width(canvas_ptr);
    int64_t tw = rt_canvas_text_width(text);
    int64_t x = rtg_sub_sat64(w, tw) / 2;
    rt_canvas_text(canvas_ptr, x, y, text, color);
}

/// @brief Draw text right-aligned to the canvas with @p margin pixels of padding.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param margin Logical distance from the canvas right edge; may be negative.
/// @param y Logical top edge of the glyph row.
/// @param text Borrowed UTF-8 runtime string.
/// @param color Packed foreground color.
void rt_canvas_text_right(
    void *canvas_ptr, int64_t margin, int64_t y, rt_string text, int64_t color) {
    if (!canvas_ptr || !text)
        return;
    int64_t w = rt_canvas_width(canvas_ptr);
    int64_t tw = rt_canvas_text_width(text);
    int64_t x = rtg_sub_sat64(rtg_sub_sat64(w, tw), margin);
    rt_canvas_text(canvas_ptr, x, y, text, color);
}

/// @brief Draw scaled text horizontally centered in the canvas at row @p y.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param y Logical top edge of the scaled glyph row.
/// @param text Borrowed UTF-8 runtime string.
/// @param color Packed foreground color.
/// @param scale Positive integer expansion factor.
void rt_canvas_text_centered_scaled(
    void *canvas_ptr, int64_t y, rt_string text, int64_t color, int64_t scale) {
    if (!canvas_ptr || !text || scale < 1)
        return;
    int64_t w = rt_canvas_width(canvas_ptr);
    int64_t tw = rt_canvas_text_scaled_width(text, scale);
    int64_t x = rtg_sub_sat64(w, tw) / 2;
    rt_canvas_text_scaled(canvas_ptr, x, y, text, scale, color);
}

//=============================================================================
// Alpha-Blended Shapes
//=============================================================================

/// @brief Fill a rectangle with @p color blended at @p alpha [0..255] over the existing pixels.
/// @details Nonpositive alpha is transparent; values at least 255 use an
///          opaque backend fill. Intermediate values perform per-pixel source-
///          over blending after logical clipping. Any alpha encoded in
///          @p color is replaced by the explicit argument.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical left edge.
/// @param y Logical top edge.
/// @param w Positive rectangle width.
/// @param h Positive rectangle height.
/// @param color Packed source RGB color.
/// @param alpha Blend alpha; values outside 0..255 clamp by branch behavior.
void rt_canvas_box_alpha(
    void *canvas_ptr, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, int64_t alpha) {
    if (!canvas_ptr || w <= 0 || h <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    if (alpha <= 0)
        return;
    if (!rt_canvas_clip_intersect_logical(canvas, &x, &y, &w, &h))
        return;
    if (alpha >= 255) {
        vgfx_fill_rect(canvas->gfx_win,
                       (int32_t)x,
                       (int32_t)y,
                       (int32_t)w,
                       (int32_t)h,
                       rt_canvas_color_to_vgfx_rgb(color));
        return;
    }

    uint32_t argb =
        ((uint32_t)(alpha & 0xFF) << 24) | ((rt_pixels_color_to_rgba(color) >> 8) & 0x00FFFFFFu);

    int64_t y_end = y + h;
    int64_t x_end = x + w;
    for (int64_t py = y; py < y_end; py++) {
        for (int64_t px = x; px < x_end; px++) {
            vgfx_pset_alpha(canvas->gfx_win, (int32_t)px, (int32_t)py, argb);
        }
    }
}

/// @brief Fill a disc with @p color blended at @p alpha [0..255] over the existing pixels.
/// @details Requires a strictly positive radius and alpha. Fully opaque int32
///          circles use the backend fast path; other cases scan only clipped
///          rows and blend individual pixels. Encoded color alpha is replaced.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param cx Logical center X.
/// @param cy Logical center Y.
/// @param radius Strictly positive logical radius.
/// @param color Packed source RGB color.
/// @param alpha Blend alpha; nonpositive is transparent and values at least
///        255 are opaque.
void rt_canvas_disc_alpha(
    void *canvas_ptr, int64_t cx, int64_t cy, int64_t radius, int64_t color, int64_t alpha) {
    if (!canvas_ptr || radius <= 0)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    if (alpha <= 0)
        return;
    if (alpha >= 255) {
        rt_canvas_disc(canvas_ptr, cx, cy, radius, color);
        return;
    }

    uint32_t argb =
        ((uint32_t)(alpha & 0xFF) << 24) | ((rt_pixels_color_to_rgba(color) >> 8) & 0x00FFFFFFu);

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return;
    int64_t y0 = rtg_sub_nonneg_sat64(cy, radius);
    int64_t y1 = rtg_add_sat64(cy, radius);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h) - 1;
    if (y0 < clip_y)
        y0 = clip_y;
    if (y1 > clip_y1)
        y1 = clip_y1;
    if (y1 < y0)
        return;

    long double r2 = (long double)radius * (long double)radius;
    for (int64_t py = y0; py <= y1; py++) {
        long double dy = (long double)py - (long double)cy;
        long double rem = r2 - dy * dy;
        if (rem < 0.0L)
            continue;
        long double dx = sqrtl(rem);
        int64_t x0 = rt_canvas_floor_ld_to_i64_sat((long double)cx - dx);
        int64_t x1 = rt_canvas_ceil_ld_to_i64_sat((long double)cx + dx);
        int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w) - 1;
        if (x0 < clip_x)
            x0 = clip_x;
        if (x1 > clip_x1)
            x1 = clip_x1;
        for (int64_t px = x0; px <= x1; px++) {
            vgfx_pset_alpha(canvas->gfx_win, (int32_t)px, (int32_t)py, argb);
            if (px == INT64_MAX)
                break;
        }
        if (py == INT64_MAX)
            break;
    }
}

//=============================================================================
// Pixel Blitting
//=============================================================================

/// @brief Clip a blit region to both source pixels bounds and destination canvas + clip rect.
/// @details In-place adjusts `(dx, dy, sx, sy, w, h)` so the resulting
///          rectangle is fully contained in:
///          1. The source `pixels` (skip the leading area where `sx`
///             or `sy` is negative; trim the trailing area that
///             extends past the source extent).
///          2. The canvas's logical clip rectangle (skip leading area
///             that lands left/above the clip; trim trailing area
///             that extends right/below).
///          Returns 0 if the resulting region is empty (caller should
///          skip the blit). Centralized here so all four blit variants
///          (opaque, region, alpha, alpha-region) share identical
///          clipping behavior — keeps clip semantics consistent and
///          off-by-one bugs to one place.
/// @param canvas Borrowed live Canvas implementation.
/// @param pixels Borrowed valid Pixels implementation with storage.
/// @param dx In/out destination logical X.
/// @param dy In/out destination logical Y.
/// @param sx In/out source X.
/// @param sy In/out source Y.
/// @param w In/out copy width.
/// @param h In/out copy height.
/// @return `1` when a positive clipped region remains; otherwise `0`.
static int8_t rt_canvas_prepare_blit_region(rt_canvas *canvas,
                                            rt_pixels_impl *pixels,
                                            int64_t *dx,
                                            int64_t *dy,
                                            int64_t *sx,
                                            int64_t *sy,
                                            int64_t *w,
                                            int64_t *h) {
    if (!canvas || !canvas->gfx_win || !pixels || !pixels->data || !dx || !dy || !sx || !sy || !w ||
        !h)
        return 0;

    if (*w <= 0 || *h <= 0)
        return 0;

    int64_t dummy_limit = INT64_MAX;
    if (!rtg_clip_copy_axis(dummy_limit, pixels->width, dx, sx, w) ||
        !rtg_clip_copy_axis(dummy_limit, pixels->height, dy, sy, h))
        return 0;

    int64_t clip_x = 0;
    int64_t clip_y = 0;
    int64_t clip_w = 0;
    int64_t clip_h = 0;
    if (!rt_canvas_get_logical_clip_bounds(canvas, &clip_x, &clip_y, &clip_w, &clip_h))
        return 0;

    if (*dx < clip_x) {
        int64_t skip = *dx == INT64_MIN ? *w : clip_x - *dx;
        if (skip >= *w)
            return 0;
        *sx += skip;
        *w -= skip;
        *dx = clip_x;
    }
    if (*dy < clip_y) {
        int64_t skip = *dy == INT64_MIN ? *h : clip_y - *dy;
        if (skip >= *h)
            return 0;
        *sy += skip;
        *h -= skip;
        *dy = clip_y;
    }

    int64_t clip_x1 = rtg_add_sat64(clip_x, clip_w);
    int64_t clip_y1 = rtg_add_sat64(clip_y, clip_h);
    if (*dx >= clip_x1 || *dy >= clip_y1)
        return 0;
    if (*w > clip_x1 - *dx)
        *w = clip_x1 - *dx;
    if (*h > clip_y1 - *dy)
        *h = clip_y1 - *dy;

    return *w > 0 && *h > 0;
}

/// @brief Copy a rectangular region from one surface to another.
/// @details Copies the full Pixels source at logical destination @p x,@p y
///          without alpha compositing: all RGBA channels overwrite the
///          framebuffer. Source/destination clipping is automatic, and each
///          logical texel expands across its HiDPI physical block.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Destination logical X.
/// @param y Destination logical Y.
/// @param pixels_ptr Borrowed Pixels handle.
void rt_canvas_blit(void *canvas_ptr, int64_t x, int64_t y, void *pixels_ptr) {
    if (!canvas_ptr || !pixels_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    rt_pixels_impl *pixels = rt_pixels_checked_impl(pixels_ptr, "Canvas.Blit: invalid pixels");
    if (!canvas || !canvas->gfx_win || !pixels || !pixels->data)
        return;

    rt_canvas_resync_window_state(canvas);

    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb) || !rtg_framebuffer_is_valid(&fb))
        return;

    int64_t dx = x;
    int64_t dy = y;
    int64_t sx = 0;
    int64_t sy = 0;
    int64_t w = pixels->width;
    int64_t h = pixels->height;
    if (!rt_canvas_prepare_blit_region(canvas, pixels, &dx, &dy, &sx, &sy, &w, &h))
        return;

    float scale = rt_canvas_effective_coord_scale(canvas);

    for (int64_t row = 0; row < h; row++) {
        int64_t py0 = rtg_scale_up_i64(dy + row, scale);
        int64_t py1 = rtg_scale_up_i64(dy + row + 1, scale);
        if (py1 <= py0)
            py1 = py0 + 1;
        if (py0 < 0)
            py0 = 0;
        if (py1 > fb.height)
            py1 = fb.height;
        if (py1 <= py0)
            continue;

        uint32_t *src_row_data = &pixels->data[(sy + row) * pixels->width + sx];
        for (int64_t col = 0; col < w; col++) {
            int64_t px0 = rtg_scale_up_i64(dx + col, scale);
            int64_t px1 = rtg_scale_up_i64(dx + col + 1, scale);
            if (px1 <= px0)
                px1 = px0 + 1;
            if (px0 < 0)
                px0 = 0;
            if (px1 > fb.width)
                px1 = fb.width;
            if (px1 <= px0)
                continue;

            uint32_t rgba = src_row_data[col];
            uint8_t r = (rgba >> 24) & 0xFF;
            uint8_t g = (rgba >> 16) & 0xFF;
            uint8_t b = (rgba >> 8) & 0xFF;
            uint8_t a = rgba & 0xFF;

            for (int64_t py = py0; py < py1; py++) {
                uint8_t *dst = &fb.pixels[(size_t)py * (size_t)fb.stride + (size_t)px0 * 4u];
                for (int64_t px = px0; px < px1; px++) {
                    dst[0] = r;
                    dst[1] = g;
                    dst[2] = b;
                    dst[3] = a;
                    dst += 4;
                }
            }
        }
    }
}

/// @brief Blit a sub-rectangle of @p pixels_ptr onto the canvas at (x, y).
/// Auto-clipped to source and destination bounds; out-of-range source rects are no-ops.
/// @details Copies RGBA bytes without alpha compositing and expands each
///          logical texel across its HiDPI physical block.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param dx Destination logical X.
/// @param dy Destination logical Y.
/// @param pixels_ptr Borrowed Pixels handle.
/// @param sx Source rectangle X.
/// @param sy Source rectangle Y.
/// @param w Source width.
/// @param h Source height.
void rt_canvas_blit_region(void *canvas_ptr,
                           int64_t dx,
                           int64_t dy,
                           void *pixels_ptr,
                           int64_t sx,
                           int64_t sy,
                           int64_t w,
                           int64_t h) {
    if (!canvas_ptr || !pixels_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    rt_pixels_impl *pixels =
        rt_pixels_checked_impl(pixels_ptr, "Canvas.BlitRegion: invalid pixels");
    if (!canvas || !canvas->gfx_win || !pixels || !pixels->data)
        return;

    rt_canvas_resync_window_state(canvas);

    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb) || !rtg_framebuffer_is_valid(&fb))
        return;

    if (!rt_canvas_prepare_blit_region(canvas, pixels, &dx, &dy, &sx, &sy, &w, &h))
        return;

    float scale = rt_canvas_effective_coord_scale(canvas);

    for (int64_t row = 0; row < h; row++) {
        int64_t py0 = rtg_scale_up_i64(dy + row, scale);
        int64_t py1 = rtg_scale_up_i64(dy + row + 1, scale);
        if (py1 <= py0)
            py1 = py0 + 1;
        if (py0 < 0)
            py0 = 0;
        if (py1 > fb.height)
            py1 = fb.height;
        if (py1 <= py0)
            continue;

        uint32_t *src_row = &pixels->data[(sy + row) * pixels->width + sx];
        for (int64_t col = 0; col < w; col++) {
            int64_t px0 = rtg_scale_up_i64(dx + col, scale);
            int64_t px1 = rtg_scale_up_i64(dx + col + 1, scale);
            if (px1 <= px0)
                px1 = px0 + 1;
            if (px0 < 0)
                px0 = 0;
            if (px1 > fb.width)
                px1 = fb.width;
            if (px1 <= px0)
                continue;

            uint32_t rgba = src_row[col];
            uint8_t r = (rgba >> 24) & 0xFF;
            uint8_t g = (rgba >> 16) & 0xFF;
            uint8_t b = (rgba >> 8) & 0xFF;
            uint8_t a = rgba & 0xFF;

            for (int64_t py = py0; py < py1; py++) {
                uint8_t *dst = &fb.pixels[(size_t)py * (size_t)fb.stride + (size_t)px0 * 4u];
                for (int64_t px = px0; px < px1; px++) {
                    dst[0] = r;
                    dst[1] = g;
                    dst[2] = b;
                    dst[3] = a;
                    dst += 4;
                }
            }
        }
    }
}

/// @brief Blit a sub-rectangle of a Pixels source with per-pixel alpha blending.
/// @details Region-cropped counterpart of rt_canvas_blit_alpha (and the blending
///          counterpart of rt_canvas_blit_region): copies the [sx,sy,w,h] source
///          rectangle to (dx,dy) compositing each texel with straight alpha rather
///          than overwriting. Internal helper (declared in rt_graphics_internal.h)
///          so the SpriteBatch region fast path can blend transparent sprite-sheet
///          frames instead of stamping opaque rectangles.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param dx Destination logical X.
/// @param dy Destination logical Y.
/// @param pixels_ptr Borrowed Pixels handle.
/// @param sx Source rectangle X.
/// @param sy Source rectangle Y.
/// @param w Source width.
/// @param h Source height.
static void rt_canvas_blit_region_alpha_raw(rt_canvas *canvas,
                                            rt_pixels_impl *pixels,
                                            const vgfx_framebuffer_t *fb,
                                            float scale,
                                            int64_t dx,
                                            int64_t dy,
                                            int64_t sx,
                                            int64_t sy,
                                            int64_t w,
                                            int64_t h) {
    if (!rt_canvas_prepare_blit_region(canvas, pixels, &dx, &dy, &sx, &sy, &w, &h))
        return;

    for (int64_t row = 0; row < h; row++) {
        int64_t py0 = rtg_scale_up_i64(dy + row, scale);
        int64_t py1 = rtg_scale_up_i64(dy + row + 1, scale);
        if (py1 <= py0)
            py1 = py0 + 1;
        if (py0 < 0)
            py0 = 0;
        if (py1 > fb->height)
            py1 = fb->height;
        if (py1 <= py0)
            continue;

        uint32_t *src_row = &pixels->data[(sy + row) * pixels->width + sx];
        for (int64_t col = 0; col < w; col++) {
            int64_t px0 = rtg_scale_up_i64(dx + col, scale);
            int64_t px1 = rtg_scale_up_i64(dx + col + 1, scale);
            if (px1 <= px0)
                px1 = px0 + 1;
            if (px0 < 0)
                px0 = 0;
            if (px1 > fb->width)
                px1 = fb->width;
            if (px1 <= px0)
                continue;

            uint32_t rgba = src_row[col];
            uint8_t sr = (rgba >> 24) & 0xFF;
            uint8_t sg = (rgba >> 16) & 0xFF;
            uint8_t sb = (rgba >> 8) & 0xFF;
            uint8_t sa = rgba & 0xFF;

            if (sa == 0)
                continue;

            for (int64_t py = py0; py < py1; py++) {
                uint8_t *dst = &fb->pixels[(size_t)py * (size_t)fb->stride + (size_t)px0 * 4u];
                for (int64_t px = px0; px < px1; px++) {
                    if (sa == 255) {
                        dst[0] = sr;
                        dst[1] = sg;
                        dst[2] = sb;
                        dst[3] = 255;
                    } else {
                        uint32_t inv_alpha = 255u - sa;
                        uint32_t da = dst[3];
                        uint32_t out_a = sa + (da * inv_alpha + 127u) / 255u;
                        if (out_a == 0) {
                            dst[0] = 0;
                            dst[1] = 0;
                            dst[2] = 0;
                            dst[3] = 0;
                        } else {
                            uint32_t r_pm = sr * sa + (dst[0] * da * inv_alpha + 127u) / 255u;
                            uint32_t g_pm = sg * sa + (dst[1] * da * inv_alpha + 127u) / 255u;
                            uint32_t b_pm = sb * sa + (dst[2] * da * inv_alpha + 127u) / 255u;
                            uint32_t r = (r_pm + out_a / 2u) / out_a;
                            uint32_t g = (g_pm + out_a / 2u) / out_a;
                            uint32_t b = (b_pm + out_a / 2u) / out_a;
                            dst[0] = (uint8_t)(r > 255u ? 255u : r);
                            dst[1] = (uint8_t)(g > 255u ? 255u : g);
                            dst[2] = (uint8_t)(b > 255u ? 255u : b);
                            dst[3] = (uint8_t)out_a;
                        }
                    }
                    dst += 4;
                }
            }
        }
    }
}

void rt_canvas_blit_region_alpha(void *canvas_ptr,
                                 int64_t dx,
                                 int64_t dy,
                                 void *pixels_ptr,
                                 int64_t sx,
                                 int64_t sy,
                                 int64_t w,
                                 int64_t h) {
    rt_canvas_alpha_region region = {dx, dy, sx, sy, w, h};
    rt_canvas_blit_regions_alpha(canvas_ptr, pixels_ptr, &region, 1u);
}

void rt_canvas_blit_regions_alpha(void *canvas_ptr,
                                  void *pixels_ptr,
                                  const rt_canvas_alpha_region *regions,
                                  size_t region_count) {
    if (!canvas_ptr || !pixels_ptr || !regions || region_count == 0)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    rt_pixels_impl *pixels =
        rt_pixels_checked_impl(pixels_ptr, "Canvas.BlitRegionsAlpha: invalid pixels");
    if (!canvas || !canvas->gfx_win || !pixels || !pixels->data)
        return;
    rt_canvas_resync_window_state(canvas);
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb) || !rtg_framebuffer_is_valid(&fb))
        return;
    float scale = rt_canvas_effective_coord_scale(canvas);
    for (size_t i = 0; i < region_count; ++i) {
        const rt_canvas_alpha_region *region = &regions[i];
        rt_canvas_blit_region_alpha_raw(canvas,
                                        pixels,
                                        &fb,
                                        scale,
                                        region->dx,
                                        region->dy,
                                        region->sx,
                                        region->sy,
                                        region->w,
                                        region->h);
    }
}

/// @brief Blit a Pixels source onto the canvas with per-pixel alpha blending.
/// @details Like `rt_canvas_blit` but each source pixel composites onto
///          the destination using straight alpha:
///          - α = 0   → skip (preserves dest exactly).
///          - α = 255 → overwrite (fast path, no division).
///          - else    → straight-alpha source-over, preserving destination alpha.
///          Honors the canvas's HiDPI scale by expanding each logical
///          source pixel into the corresponding physical pixel block
///          (so a 1× sprite stays sharp on a 2× display).
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Destination logical X.
/// @param y Destination logical Y.
/// @param pixels_ptr Borrowed Pixels handle.
void rt_canvas_blit_alpha(void *canvas_ptr, int64_t x, int64_t y, void *pixels_ptr) {
    if (!canvas_ptr || !pixels_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    rt_pixels_impl *pixels = rt_pixels_checked_impl(pixels_ptr, "Canvas.BlitAlpha: invalid pixels");
    if (!canvas || !canvas->gfx_win || !pixels || !pixels->data)
        return;

    rt_canvas_resync_window_state(canvas);

    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb) || !rtg_framebuffer_is_valid(&fb))
        return;

    int64_t dx = x;
    int64_t dy = y;
    int64_t sx = 0;
    int64_t sy = 0;
    int64_t w = pixels->width;
    int64_t h = pixels->height;
    if (!rt_canvas_prepare_blit_region(canvas, pixels, &dx, &dy, &sx, &sy, &w, &h))
        return;

    float scale = rt_canvas_effective_coord_scale(canvas);

    for (int64_t row = 0; row < h; row++) {
        int64_t py0 = rtg_scale_up_i64(dy + row, scale);
        int64_t py1 = rtg_scale_up_i64(dy + row + 1, scale);
        if (py1 <= py0)
            py1 = py0 + 1;
        if (py0 < 0)
            py0 = 0;
        if (py1 > fb.height)
            py1 = fb.height;
        if (py1 <= py0)
            continue;

        uint32_t *src_row = &pixels->data[(sy + row) * pixels->width + sx];
        for (int64_t col = 0; col < w; col++) {
            int64_t px0 = rtg_scale_up_i64(dx + col, scale);
            int64_t px1 = rtg_scale_up_i64(dx + col + 1, scale);
            if (px1 <= px0)
                px1 = px0 + 1;
            if (px0 < 0)
                px0 = 0;
            if (px1 > fb.width)
                px1 = fb.width;
            if (px1 <= px0)
                continue;

            uint32_t rgba = src_row[col];
            uint8_t sr = (rgba >> 24) & 0xFF;
            uint8_t sg = (rgba >> 16) & 0xFF;
            uint8_t sb = (rgba >> 8) & 0xFF;
            uint8_t sa = rgba & 0xFF;

            if (sa == 0)
                continue;

            for (int64_t py = py0; py < py1; py++) {
                uint8_t *dst = &fb.pixels[(size_t)py * (size_t)fb.stride + (size_t)px0 * 4u];
                for (int64_t px = px0; px < px1; px++) {
                    if (sa == 255) {
                        dst[0] = sr;
                        dst[1] = sg;
                        dst[2] = sb;
                        dst[3] = 255;
                    } else {
                        uint32_t inv_alpha = 255u - sa;
                        uint32_t da = dst[3];
                        uint32_t out_a = sa + (da * inv_alpha + 127u) / 255u;
                        if (out_a == 0) {
                            dst[0] = 0;
                            dst[1] = 0;
                            dst[2] = 0;
                            dst[3] = 0;
                        } else {
                            uint32_t r_pm = sr * sa + (dst[0] * da * inv_alpha + 127u) / 255u;
                            uint32_t g_pm = sg * sa + (dst[1] * da * inv_alpha + 127u) / 255u;
                            uint32_t b_pm = sb * sa + (dst[2] * da * inv_alpha + 127u) / 255u;
                            uint32_t r = (r_pm + out_a / 2u) / out_a;
                            uint32_t g = (g_pm + out_a / 2u) / out_a;
                            uint32_t b = (b_pm + out_a / 2u) / out_a;
                            dst[0] = (uint8_t)(r > 255u ? 255u : r);
                            dst[1] = (uint8_t)(g > 255u ? 255u : g);
                            dst[2] = (uint8_t)(b > 255u ? 255u : b);
                            dst[3] = (uint8_t)out_a;
                        }
                    }
                    dst += 4;
                }
            }
        }
    }
}

//=============================================================================
// Canvas Utilities (get_pixel, copy_rect, save_bmp, save_png)
//=============================================================================

/// @brief Read a single pixel's color from the canvas at the given logical coordinates.
/// @details Returns the packed 0xRRGGBB color at (x, y), or 0 if the
///          canvas isn't ready or (x, y) is out of bounds. Useful for
///          procedural painting tools and color picking. Goes through
///          `vgfx_point` rather than direct framebuffer access so the
///          read honors any pending coordinate transforms.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical X coordinate.
/// @param y Logical Y coordinate.
/// @return Backend packed RGB value, or `0` for invalid/out-of-range input.
int64_t rt_canvas_get_pixel(void *canvas_ptr, int64_t x, int64_t y) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas)
        return 0;
    if (!canvas->gfx_win)
        return 0;

    rt_canvas_resync_window_state(canvas);
    if (!rtg_i64_fits_i32(x) || !rtg_i64_fits_i32(y))
        return 0;

    vgfx_color_t color;
    if (vgfx_point(canvas->gfx_win, (int32_t)x, (int32_t)y, &color) != 0) {
        return (int64_t)color;
    }
    return 0;
}

/// @brief Copy a rectangular region of the canvas into a freshly allocated Pixels object.
/// @details Goes through the live framebuffer rather than per-pixel
///          `vgfx_point` calls — for an `N×M` rect that's `O(N*M)`
///          instead of `O(N*M)` clipped queries, easily 10× faster
///          on large screenshots. HiDPI: each logical pixel samples
///          its scaled top-left physical pixel, so the returned
///          Pixels stays at logical resolution (matches what the
///          user drew, not what the GPU rendered). Out-of-range
///          source pixels are recorded as 0 so the resulting buffer
///          is dense and easy to feed back into `Canvas.Blit`. Requests
///          larger than 268,435,456 pixels are rejected.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Logical source left edge.
/// @param y Logical source top edge.
/// @param w Positive output width.
/// @param h Positive output height.
/// @return Owned Pixels snapshot, or `NULL` for invalid dimensions/handles,
///         size overflow, allocation failure, or framebuffer failure.
void *rt_canvas_copy_rect(void *canvas_ptr, int64_t x, int64_t y, int64_t w, int64_t h) {
    if (w <= 0 || h <= 0)
        return NULL;
    if (w > INT64_MAX / h || w * h > INT64_C(268435456))
        return NULL;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas)
        return NULL;
    if (!canvas->gfx_win)
        return NULL;

    rt_canvas_resync_window_state(canvas);

    // Create a new Pixels buffer
    void *pixels = rt_pixels_new(w, h);
    if (!pixels)
        return NULL;

    // Copy pixels from canvas to buffer via direct framebuffer access — avoids
    // O(w*h) vgfx_point() calls (each involves clipping + bounds checking).
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas->gfx_win, &fb) || !rtg_framebuffer_is_valid(&fb)) {
        if (rt_obj_release_check0(pixels))
            rt_obj_free(pixels);
        return NULL;
    }

    // Sample the physical pixel at the logical pixel's scaled top-left corner.
    float scale = rt_canvas_effective_coord_scale(canvas);

    rt_pixels_impl *pix = rt_pixels_checked_impl_or_null(pixels);
    if (!pix || !pix->data) {
        if (rt_obj_release_check0(pixels))
            rt_obj_free(pixels);
        return NULL;
    }

    for (int64_t py = 0; py < h; py++) {
        int64_t src_y = rtg_scale_up_i64(rtg_add_sat64(y, py), scale);
        if (src_y < 0 || src_y >= fb.height)
            continue;

        uint8_t *src_row = &fb.pixels[(size_t)src_y * (size_t)fb.stride];
        uint32_t *dst_row = &pix->data[(size_t)(py * w)];

        for (int64_t px = 0; px < w; px++) {
            int64_t src_x = rtg_scale_up_i64(rtg_add_sat64(x, px), scale);
            if (src_x < 0 || src_x >= fb.width) {
                dst_row[px] = 0;
                continue;
            }
            uint8_t r = src_row[(size_t)src_x * 4u + 0u];
            uint8_t g = src_row[(size_t)src_x * 4u + 1u];
            uint8_t b = src_row[(size_t)src_x * 4u + 2u];
            uint8_t a = src_row[(size_t)src_x * 4u + 3u];
            dst_row[px] =
                ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
        }
    }

    return pixels;
}

/// @brief Save the canvas contents to a 24-bit BMP file at `path`.
/// @details Implementation steps:
///          1. Snapshot the live canvas via `rt_canvas_copy_rect`
///             into a temporary Pixels object.
///          2. Delegate to `rt_pixels_save_bmp` for the actual file
///             write (handles BMP header, row alignment, alpha→24-bit
///             demotion).
///          3. Release the temporary Pixels after the write completes.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param path Borrowed output path runtime string.
/// @return `1` on success; `0` on invalid input, snapshot, or write failure.
int64_t rt_canvas_save_bmp(void *canvas_ptr, rt_string path) {
    if (!path)
        return 0;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas)
        return 0;
    if (!canvas->gfx_win)
        return 0;

    int32_t w, h;
    if (vgfx_get_size(canvas->gfx_win, &w, &h) == 0)
        return 0;

    // Create a temporary Pixels buffer with canvas contents
    void *pixels = rt_canvas_copy_rect(canvas_ptr, 0, 0, w, h);
    if (!pixels)
        return 0;

    int64_t result = rt_pixels_save_bmp(pixels, path);
    if (rt_obj_release_check0(pixels))
        rt_obj_free(pixels);

    return result;
}

/// @brief Save the canvas contents to a PNG file at `path`.
/// @details Same flow as `_save_bmp` but routes to `rt_pixels_save_png`
///          which produces a deflate-compressed RGBA PNG (preserving
///          per-pixel alpha unlike the BMP path). Smaller files than
///          BMP for typical UI screenshots; slower to write because
///          of the compression pass.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param path Borrowed output path runtime string.
/// @return `1` on success; `0` on invalid input, snapshot, or write failure.
int64_t rt_canvas_save_png(void *canvas_ptr, rt_string path) {
    if (!path)
        return 0;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas)
        return 0;
    if (!canvas->gfx_win)
        return 0;

    int32_t w, h;
    if (vgfx_get_size(canvas->gfx_win, &w, &h) == 0)
        return 0;

    void *pixels = rt_canvas_copy_rect(canvas_ptr, 0, 0, w, h);
    if (!pixels)
        return 0;

    int64_t result = rt_pixels_save_png(pixels, path);
    if (rt_obj_release_check0(pixels))
        rt_obj_free(pixels);
    return result;
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
