//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_overlay.c
// Purpose: Canvas3D screen-space overlay, screenshot, and debug-draw helpers.
//   Implements Zanna.Graphics3D.Canvas3D's debug visualizers (lines, points,
//   AABB / sphere wireframes, axis gizmos), HUD primitives (rect, crosshair,
//   text), backend-capability queries, and the screenshot capture path.
//
// Key invariants:
//   - All overlay draws automatically open and close a temporary overlay
//     frame when called outside of an explicit Begin/End bracket.
//   - 3D-anchored overlays project through `canvas3d_active_scene_vp` so
//     gizmos drawn after `End` stay anchored to the scene that was just
//     rendered.
//   - Screenshot RGBA packing follows `rt_pixels`'s 0xRRGGBBAA convention
//     so captured images can be saved without a swizzle pass.
//
// Ownership/Lifetime:
//   - Helpers borrow the canvas / Vec3 inputs; locally constructed Vec3s
//     are released via `canvas3d_release_local` before returning.
//   - `rt_canvas3d_screenshot` returns a freshly allocated Pixels object;
//     the caller takes ownership.
//
// Links: rt_canvas3d.h, rt_canvas3d_internal.h, vgfx3d_backend.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Canvas3D HUD overlays, debug drawing, capability queries, and screenshots.
/// @details Helpers project retained scene geometry into logical screen space,
///   queue post-scene primitives with bounded raster caches, expose renderer
///   diagnostics and capability truth, and convert backend readback into the
///   runtime Pixels packing convention.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_font.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_string.h"
#include "rt_textureasset3d.h"
#include "rt_ttf_font.h"
#include "rt_vec3.h"
#include "vg_font.h"
#include "vgfx3d_backend.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Internal TextureAsset3D capability query used to back Canvas3D.BackendSupports.
extern int8_t rt_textureasset3d_cpu_supports_format(const char *format_name);
/// @brief Internal KTX2 parser/fallback availability query used by Canvas3D.BackendSupports.
extern int8_t rt_textureasset3d_cpu_supports_ktx2(void);

/* DrawText2DAA produces CPU-rasterized Pixels. Bound both entry count and retained bytes so
 * dynamic labels (timers, coordinates, chat) cannot grow the cache without limit. Typical HUD
 * text occupies only a few KiB, while unusually large one-off strings keep the frame-local path. */
#define CANVAS3D_AA_TEXT_CACHE_MAX_ENTRIES 128
#define CANVAS3D_AA_TEXT_CACHE_MAX_BYTES (8u * 1024u * 1024u)
#define CANVAS3D_AA_TEXT_CACHE_MAX_ENTRY_BYTES (2u * 1024u * 1024u)

/// @brief Copy the bounded, complete-codepoint prefix consumed by Canvas3D TTF rendering.
/// @details Runtime strings carry a byte length and may contain embedded NULs. The vg font API is
///          NUL-terminated, so this helper stops at the first NUL and never cuts a well-formed
///          multi-byte UTF-8 sequence at the renderer's byte ceiling. Malformed bytes remain
///          single-byte input and are converted to U+FFFD by `vg_utf8_decode`.
/// @param text Borrowed runtime string.
/// @param out Writable buffer of at least `RT_CANVAS3D_TEXT_MAX_BYTES + 1` bytes.
/// @return Number of copied bytes, excluding the terminator.
static size_t canvas3d_copy_ttf_text_prefix(rt_string text,
                                            char out[RT_CANVAS3D_TEXT_MAX_BYTES + 1u]) {
    const char *source;
    int64_t stored_len;
    size_t limit;
    size_t cursor = 0;
    if (!out)
        return 0;
    out[0] = '\0';
    if (!text)
        return 0;
    source = rt_string_cstr(text);
    stored_len = rt_str_len(text);
    if (!source || stored_len <= 0)
        return 0;
    limit = (size_t)stored_len;
    if (limit > RT_CANVAS3D_TEXT_MAX_BYTES)
        limit = RT_CANVAS3D_TEXT_MAX_BYTES;
    while (cursor < limit && source[cursor] != '\0') {
        const unsigned char lead = (unsigned char)source[cursor];
        size_t sequence_bytes = 1;
        if ((lead & 0xE0u) == 0xC0u)
            sequence_bytes = 2;
        else if ((lead & 0xF0u) == 0xE0u)
            sequence_bytes = 3;
        else if ((lead & 0xF8u) == 0xF0u)
            sequence_bytes = 4;
        if (sequence_bytes > 1) {
            int complete = cursor + sequence_bytes <= limit;
            for (size_t i = 1; complete && i < sequence_bytes; i++)
                complete = ((unsigned char)source[cursor + i] & 0xC0u) == 0x80u;
            if (!complete) {
                if (cursor + sequence_bytes > limit)
                    break;
                sequence_bytes = 1;
            }
        }
        cursor += sequence_bytes;
    }
    memcpy(out, source, cursor);
    out[cursor] = '\0';
    return cursor;
}

/// @brief Drop one AA text cache entry and compact the live prefix.
/// @param c Canvas3D owning the retained raster and key storage.
/// @param index Zero-based live entry to release.
static void canvas3d_remove_aa_text_cache_entry(rt_canvas3d *c, int32_t index) {
    rt_canvas3d_aa_text_cache_entry *entry;
    int32_t last;

    if (!c || !c->aa_text_cache || index < 0 || index >= c->aa_text_cache_count)
        return;
    entry = &c->aa_text_cache[index];
    if (entry->pixels && rt_obj_release_check0(entry->pixels))
        rt_obj_free(entry->pixels);
    free(entry->text);
    if (entry->retained_bytes <= c->aa_text_cache_bytes)
        c->aa_text_cache_bytes -= entry->retained_bytes;
    else
        c->aa_text_cache_bytes = 0;

    last = c->aa_text_cache_count - 1;
    if (index != last)
        c->aa_text_cache[index] = c->aa_text_cache[last];
    memset(&c->aa_text_cache[last], 0, sizeof(c->aa_text_cache[last]));
    c->aa_text_cache_count = last;
}

/// @brief Find the least-recently-used AA text raster.
/// @param c Borrowed Canvas3D containing cache usage stamps.
/// @return Zero-based oldest entry, or negative one for an empty/invalid cache.
static int32_t canvas3d_oldest_aa_text_cache_entry(const rt_canvas3d *c) {
    int32_t oldest = 0;

    if (!c || c->aa_text_cache_count <= 0)
        return -1;
    for (int32_t i = 1; i < c->aa_text_cache_count; i++) {
        if (c->aa_text_cache[i].last_used_frame < c->aa_text_cache[oldest].last_used_frame)
            oldest = i;
    }
    return oldest;
}

/// @brief Release all persistent AA text rasters owned by a canvas.
/// @param c Canvas3D whose raster references, text keys, and cache array are freed.
void canvas3d_clear_aa_text_cache(rt_canvas3d *c) {
    if (!c)
        return;
    while (c->aa_text_cache_count > 0)
        canvas3d_remove_aa_text_cache_entry(c, c->aa_text_cache_count - 1);
    free(c->aa_text_cache);
    c->aa_text_cache = NULL;
    c->aa_text_cache_bytes = 0;
}

/// @brief Return a cached raster that exactly matches the rendered AA text inputs.
/// @param c Canvas3D whose cache is searched and usage stamp updated.
/// @param text Borrowed UTF-8 byte sequence.
/// @param text_len Number of key bytes in @p text.
/// @param color Packed runtime text color.
/// @param scale Exact raster scale key.
/// @param width Expected raster width in pixels.
/// @param height Expected raster height in pixels.
/// @return Borrowed cached Pixels handle, or `NULL` when no exact entry exists.
static void *canvas3d_find_aa_text_cache_entry(rt_canvas3d *c,
                                               const char *text,
                                               size_t text_len,
                                               int64_t color,
                                               double scale,
                                               int32_t width,
                                               int32_t height,
                                               int64_t font_identity) {
    if (!c || !text)
        return NULL;
    for (int32_t i = 0; i < c->aa_text_cache_count; i++) {
        rt_canvas3d_aa_text_cache_entry *entry = &c->aa_text_cache[i];
        if (entry->text_len != text_len || entry->color != color || entry->scale != scale ||
            entry->width != width || entry->height != height ||
            entry->font_identity != font_identity || !entry->text ||
            memcmp(entry->text, text, text_len) != 0)
            continue;
        entry->last_used_frame = c->frame_serial;
        return entry->pixels;
    }
    return NULL;
}

/// @brief Make @p pixels the persistent raster for one rendered AA label.
/// @param c Canvas3D owning the bounded cache.
/// @param text Borrowed UTF-8 byte sequence copied into the key.
/// @param text_len Number of key bytes in @p text.
/// @param color Packed runtime text color.
/// @param scale Exact raster scale key.
/// @param width Positive raster width in pixels.
/// @param height Positive raster height in pixels.
/// @param pixels Owned Pixels reference transferred only on success.
/// @return 1 when the cache owns the Pixels reference, or 0 for frame-local fallback.
static int canvas3d_insert_aa_text_cache_entry(rt_canvas3d *c,
                                               const char *text,
                                               size_t text_len,
                                               int64_t color,
                                               double scale,
                                               int32_t width,
                                               int32_t height,
                                               int64_t font_identity,
                                               void *pixels) {
    rt_canvas3d_aa_text_cache_entry *entry;
    size_t pixel_count;
    size_t retained_bytes;
    char *text_copy;

    if (!c || !text || !pixels || width <= 0 || height <= 0)
        return 0;
    if ((size_t)width > SIZE_MAX / (size_t)height)
        return 0;
    pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > (SIZE_MAX - text_len - 1u) / 4u)
        return 0;
    retained_bytes = pixel_count * 4u + text_len + 1u;
    if (retained_bytes > CANVAS3D_AA_TEXT_CACHE_MAX_ENTRY_BYTES ||
        retained_bytes > CANVAS3D_AA_TEXT_CACHE_MAX_BYTES)
        return 0;

    if (!c->aa_text_cache) {
        c->aa_text_cache = (rt_canvas3d_aa_text_cache_entry *)calloc(
            CANVAS3D_AA_TEXT_CACHE_MAX_ENTRIES, sizeof(*c->aa_text_cache));
        if (!c->aa_text_cache)
            return 0;
    }
    while (c->aa_text_cache_count >= CANVAS3D_AA_TEXT_CACHE_MAX_ENTRIES ||
           c->aa_text_cache_bytes > CANVAS3D_AA_TEXT_CACHE_MAX_BYTES - retained_bytes) {
        int32_t oldest = canvas3d_oldest_aa_text_cache_entry(c);
        if (oldest < 0)
            return 0;
        canvas3d_remove_aa_text_cache_entry(c, oldest);
    }

    text_copy = (char *)malloc(text_len + 1u);
    if (!text_copy)
        return 0;
    memcpy(text_copy, text, text_len);
    text_copy[text_len] = '\0';

    entry = &c->aa_text_cache[c->aa_text_cache_count++];
    entry->text = text_copy;
    entry->text_len = text_len;
    entry->pixels = pixels;
    entry->retained_bytes = retained_bytes;
    entry->scale = scale;
    entry->color = color;
    entry->last_used_frame = c->frame_serial;
    entry->font_identity = font_identity;
    entry->width = width;
    entry->height = height;
    c->aa_text_cache_bytes += retained_bytes;
    return 1;
}

/// @brief Project a 3D world-space point onto 2D screen coordinates using the active scene VP.
/// @details Standard `world → clip → NDC → screen` pipeline:
///          1. Multiply (wp.x, wp.y, wp.z, 1) by the cached view-projection
///             matrix to land in homogeneous clip space.
///          2. Reject points behind the camera (`clip.w <= 0`) — those
///             would invert through the perspective divide.
///          3. Perspective-divide to NDC, remap [-1,1] → [0, fb_w/h], and
///             flip Y so origin sits at the top-left to match screen
///             conventions.
///          Uses `canvas3d_active_scene_vp` so debug overlays projected
///          *after* `End` still use the same VP that drew the scene —
///          markers stay anchored across the begin/end boundary.
///          Returns 0 if no scene VP is available or the point is behind
///          the camera (caller should skip the draw).
/// @param c Borrowed Canvas3D supplying the retained scene view-projection matrix.
/// @param wp Borrowed three-component world position.
/// @param sx Non-`NULL` horizontal screen-coordinate output.
/// @param sy Non-`NULL` vertical screen-coordinate output.
/// @param fb_w Positive logical output width.
/// @param fb_h Positive logical output height.
/// @return Non-zero when the point lies in front of an available scene camera.
static int world_to_screen(
    const rt_canvas3d *c, const float *wp, float *sx, float *sy, int32_t fb_w, int32_t fb_h) {
    const float *vp = canvas3d_active_scene_vp(c);
    float pos4[4];
    float clip[4];
    if (!vp || !wp || !sx || !sy || fb_w <= 0 || fb_h <= 0 || !isfinite(wp[0]) ||
        !isfinite(wp[1]) || !isfinite(wp[2]))
        return 0;
    pos4[0] = wp[0];
    pos4[1] = wp[1];
    pos4[2] = wp[2];
    pos4[3] = 1.0f;
    clip[0] = vp[0] * pos4[0] + vp[1] * pos4[1] + vp[2] * pos4[2] + vp[3] * pos4[3];
    clip[1] = vp[4] * pos4[0] + vp[5] * pos4[1] + vp[6] * pos4[2] + vp[7] * pos4[3];
    clip[2] = vp[8] * pos4[0] + vp[9] * pos4[1] + vp[10] * pos4[2] + vp[11] * pos4[3];
    clip[3] = vp[12] * pos4[0] + vp[13] * pos4[1] + vp[14] * pos4[2] + vp[15] * pos4[3];
    if (!isfinite(clip[0]) || !isfinite(clip[1]) || !isfinite(clip[2]) || !isfinite(clip[3]) ||
        clip[3] <= 0.0f)
        return 0;
    {
        const float iw = 1.0f / clip[3];
        *sx = (clip[0] * iw + 1.0f) * 0.5f * (float)fb_w;
        *sy = (1.0f - clip[1] * iw) * 0.5f * (float)fb_h;
    }
    return isfinite(*sx) && isfinite(*sy);
}

/// @brief Resolve the active output surface's public coordinate size.
/// @details When a render target is bound, overlays size to the RTT. Otherwise
///          they size to the Canvas3D logical dimensions, not the framebuffer
///          backing size, so HiDPI windows keep stable public coordinates.
/// @param c Borrowed Canvas3D whose active output is inspected.
/// @param out_w Optional output initialized and set to logical width.
/// @param out_h Optional output initialized and set to logical height.
/// @return Non-zero when both resolved dimensions are positive.
static int overlay_output_size(const rt_canvas3d *c, int32_t *out_w, int32_t *out_h) {
    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;
    if (!c)
        return 0;
    if (c->render_target) {
        if (out_w)
            *out_w = c->render_target->width;
        if (out_h)
            *out_h = c->render_target->height;
        return c->render_target->width > 0 && c->render_target->height > 0;
    }
    if (out_w)
        *out_w = c->width;
    if (out_h)
        *out_h = c->height;
    return c->width > 0 && c->height > 0;
}

/// @brief Convert retained float coordinates back to public integers without out-of-range casts.
/// @param value Finite or corrupt floating-point coordinate.
/// @return Truncated coordinate saturated to the signed 64-bit domain; non-finite values return 0.
static int64_t canvas3d_overlay_float_to_i64(float value) {
    const double wide = (double)value;
    if (!isfinite(value))
        return 0;
    if (wide >= (double)INT64_MAX)
        return INT64_MAX;
    if (wide <= (double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)wide;
}

/// @brief Pack an RGBA byte surface into `Pixels`, scaling to logical size when needed.
/// @details Equal dimensions copy directly; differing dimensions use box
///   averaging so high-DPI readback downsamples without channel swizzling.
/// @param pv Mutable Pixels payload with @p dst_w by @p dst_h storage.
/// @param src Borrowed RGBA8 source bytes.
/// @param src_w Positive source width.
/// @param src_h Positive source height.
/// @param src_stride Source row stride in bytes.
/// @param dst_w Positive destination width.
/// @param dst_h Positive destination height.
/// @return Non-zero when the full image is packed successfully.
static int canvas3d_pack_rgba_to_pixels(rt_pixels_impl *pv,
                                        const uint8_t *src,
                                        int32_t src_w,
                                        int32_t src_h,
                                        int32_t src_stride,
                                        int32_t dst_w,
                                        int32_t dst_h) {
    if (!pv || !pv->data || !src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return 0;
    if ((int64_t)src_stride < (int64_t)src_w * 4)
        return 0;

    if (src_w == dst_w && src_h == dst_h) {
        for (int32_t y = 0; y < dst_h; y++) {
            for (int32_t x = 0; x < dst_w; x++) {
                const uint8_t *p = src + (size_t)y * (size_t)src_stride + (size_t)x * 4u;
                pv->data[(size_t)y * (size_t)pv->width + (size_t)x] =
                    ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
                    (uint32_t)p[3];
            }
        }
        return 1;
    }

    for (int32_t y = 0; y < dst_h; y++) {
        int32_t y0 = (int32_t)(((int64_t)y * src_h) / dst_h);
        int32_t y1 = (int32_t)(((int64_t)(y + 1) * src_h) / dst_h);
        if (y1 <= y0)
            y1 = y0 + 1;
        if (y1 > src_h)
            y1 = src_h;
        for (int32_t x = 0; x < dst_w; x++) {
            int32_t x0 = (int32_t)(((int64_t)x * src_w) / dst_w);
            int32_t x1 = (int32_t)(((int64_t)(x + 1) * src_w) / dst_w);
            uint64_t r = 0;
            uint64_t g = 0;
            uint64_t b = 0;
            uint64_t a = 0;
            uint64_t count = 0;
            if (x1 <= x0)
                x1 = x0 + 1;
            if (x1 > src_w)
                x1 = src_w;
            for (int32_t sy = y0; sy < y1; sy++) {
                const uint8_t *row = src + (size_t)sy * (size_t)src_stride;
                for (int32_t sx = x0; sx < x1; sx++) {
                    const uint8_t *p = row + (size_t)sx * 4u;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    a += p[3];
                    count++;
                }
            }
            if (count == 0)
                count = 1;
            pv->data[(size_t)y * (size_t)pv->width + (size_t)x] =
                ((uint32_t)((r + count / 2u) / count) << 24) |
                ((uint32_t)((g + count / 2u) / count) << 16) |
                ((uint32_t)((b + count / 2u) / count) << 8) | (uint32_t)((a + count / 2u) / count);
        }
    }
    return 1;
}

/// @brief Drop one reference and free if zero. Safe on NULL.
/// @param obj Owned local runtime reference to release.
static void canvas3d_release_local(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Draw a 3D world-space line between two Vec3 endpoints in `color`. Useful for debug
/// visualizers, motion trails, gizmos. Color is 0xRRGGBBAA. Auto-projects to screen space.
/// @param obj Borrowed Canvas3D handle.
/// @param from Borrowed three-double world-space start point.
/// @param to Borrowed three-double world-space end point.
/// @param color Packed RGB runtime color; overlay alpha is opaque.
void rt_canvas3d_draw_line3d_raw(void *obj, const double *from, const double *to, int64_t color) {
    int8_t started_temp_frame = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !from || !to)
        return;
    int32_t out_w = 0;
    int32_t out_h = 0;
    if (!overlay_output_size(c, &out_w, &out_h))
        return;

    {
        float p0[3] = {(float)from[0], (float)from[1], (float)from[2]};
        float p1[3] = {(float)to[0], (float)to[1], (float)to[2]};
        float sx0;
        float sy0;
        float sx1;
        float sy1;
        if (!world_to_screen(c, p0, &sx0, &sy0, out_w, out_h))
            return;
        if (!world_to_screen(c, p1, &sx1, &sy1, out_w, out_h))
            return;
        if (!c->in_frame) {
            if (!canvas3d_begin_overlay_frame(c, 1))
                return;
            started_temp_frame = 1;
        }
        (void)canvas3d_queue_screen_line(c,
                                         sx0,
                                         sy0,
                                         sx1,
                                         sy1,
                                         1.0f,
                                         (float)((color >> 16) & 0xFF) / 255.0f,
                                         (float)((color >> 8) & 0xFF) / 255.0f,
                                         (float)(color & 0xFF) / 255.0f,
                                         1.0f);
    }
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw a 3D line segment between two Vec3 world points in the given packed color.
/// @param obj Borrowed Canvas3D handle.
/// @param from Borrowed Vec3 start point.
/// @param to Borrowed Vec3 end point.
/// @param color Packed RGB runtime color.
void rt_canvas3d_draw_line3d(void *obj, void *from, void *to, int64_t color) {
    double p0[3];
    double p1[3];
    if (!from || !to)
        return;
    p0[0] = rt_vec3_x(from);
    p0[1] = rt_vec3_y(from);
    p0[2] = rt_vec3_z(from);
    p1[0] = rt_vec3_x(to);
    p1[1] = rt_vec3_y(to);
    p1[2] = rt_vec3_z(to);
    rt_canvas3d_draw_line3d_raw(obj, p0, p1, color);
}

/// @brief Draw a 3D world-space point at `pos` (Vec3) as a `size`-pixel filled square in `color`.
/// Useful for marking spawn points, raycast hits, AI waypoints during debug.
/// @param obj Borrowed Canvas3D handle.
/// @param pos Borrowed Vec3 world position.
/// @param color Packed RGB runtime color.
/// @param size Requested square side length in pixels; non-positive values become one.
void rt_canvas3d_draw_point3d(void *obj, void *pos, int64_t color, int64_t size) {
    int8_t started_temp_frame = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !pos)
        return;
    int32_t out_w = 0;
    int32_t out_h = 0;
    if (!overlay_output_size(c, &out_w, &out_h))
        return;

    {
        float p[3] = {(float)rt_vec3_x(pos), (float)rt_vec3_y(pos), (float)rt_vec3_z(pos)};
        float sx;
        float sy;
        if (!world_to_screen(c, p, &sx, &sy, out_w, out_h))
            return;
        if (!c->in_frame) {
            if (!canvas3d_begin_overlay_frame(c, 1))
                return;
            started_temp_frame = 1;
        }
        {
            const float side = size > 0 ? (float)size : 1.0f;
            const float half = side * 0.5f;
            (void)canvas3d_queue_screen_rect(c,
                                             sx - half,
                                             sy - half,
                                             side,
                                             side,
                                             (float)((color >> 16) & 0xFF) / 255.0f,
                                             (float)((color >> 8) & 0xFF) / 255.0f,
                                             (float)(color & 0xFF) / 255.0f,
                                             1.0f);
        }
    }
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw a screen-space (2D, ignores 3D camera) filled rectangle. Useful for HUDs and
/// debug overlays composited over the 3D scene.
/// @param obj Borrowed Canvas3D handle.
/// @param x Left edge in logical pixels.
/// @param y Top edge in logical pixels.
/// @param w Width in logical pixels.
/// @param h Height in logical pixels.
/// @param color Packed RGB runtime color.
void rt_canvas3d_draw_rect2d(void *obj, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    int8_t started_temp_frame = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (w <= 0 || h <= 0)
        return;
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }
    rt_canvas3d_draw_rect_3d(c, x, y, w, h, color);
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw a screen-space filled rectangle with explicit opacity.
/// @details Like `DrawRect2D` but blends with the scene: `alpha` 0..1 (values
///   are clamped). The workhorse for HUD panels and full-screen fade overlays.
/// @param obj Borrowed Canvas3D handle.
/// @param x Left edge in logical pixels.
/// @param y Top edge in logical pixels.
/// @param w Width in logical pixels.
/// @param h Height in logical pixels.
/// @param color Packed RGB runtime color.
/// @param alpha Normalized opacity; invalid values are sanitized by queueing.
void rt_canvas3d_draw_rect2d_alpha(
    void *obj, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, double alpha) {
    int8_t started_temp_frame = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (w <= 0 || h <= 0)
        return;
    if (!isfinite(alpha))
        alpha = 1.0;
    if (alpha < 0.0)
        alpha = 0.0;
    if (alpha > 1.0)
        alpha = 1.0;
    if (alpha <= 0.0001)
        return;
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }
    (void)canvas3d_queue_screen_rect(c,
                                     (float)x,
                                     (float)y,
                                     (float)w,
                                     (float)h,
                                     (float)((color >> 16) & 0xFF) / 255.0f,
                                     (float)((color >> 8) & 0xFF) / 255.0f,
                                     (float)(color & 0xFF) / 255.0f,
                                     (float)alpha);
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Blit a `Pixels` image into the 2D overlay at (x,y) scaled to (w,h).
/// @details Screen-space, unlit, ignores the 3D camera — composites over the scene like
///   `DrawRect2D`/`DrawText2D`. Pair with `RenderTarget3D.AsPixels` to display a rendered
///   texture (e.g. a top-down minimap) on the HUD. NULL- and empty-rect-safe.
/// @param obj Borrowed Canvas3D handle.
/// @param x Left destination edge in logical pixels.
/// @param y Top destination edge in logical pixels.
/// @param w Destination width in logical pixels.
/// @param h Destination height in logical pixels.
/// @param pixels Borrowed Pixels handle retained through deferred replay.
void rt_canvas3d_draw_image2d(void *obj, int64_t x, int64_t y, int64_t w, int64_t h, void *pixels) {
    int8_t started_temp_frame = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !pixels)
        return;
    if (w <= 0 || h <= 0)
        return;
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }
    (void)canvas3d_queue_screen_image(c, (float)x, (float)y, (float)w, (float)h, pixels);
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw a screen-space line segment (thickness 1) with explicit opacity (Plan 08).
/// @param obj Borrowed Canvas3D handle.
/// @param x0 First endpoint X in logical pixels.
/// @param y0 First endpoint Y in logical pixels.
/// @param x1 Second endpoint X in logical pixels.
/// @param y1 Second endpoint Y in logical pixels.
/// @param color Packed RGB runtime color.
/// @param alpha Normalized opacity.
void rt_canvas3d_draw_line2d(
    void *obj, int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t color, double alpha) {
    int8_t started_temp_frame = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!isfinite(alpha))
        alpha = 1.0;
    if (alpha < 0.0)
        alpha = 0.0;
    if (alpha > 1.0)
        alpha = 1.0;
    if (alpha <= 0.0001)
        return;
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }
    (void)canvas3d_queue_screen_line(c,
                                     (float)x0,
                                     (float)y0,
                                     (float)x1,
                                     (float)y1,
                                     1.0f,
                                     (float)((color >> 16) & 0xFF) / 255.0f,
                                     (float)((color >> 8) & 0xFF) / 255.0f,
                                     (float)(color & 0xFF) / 255.0f,
                                     (float)alpha);
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw a screen-space 1px rectangle outline with explicit opacity (Plan 08).
/// @param obj Borrowed Canvas3D handle.
/// @param x Left edge in logical pixels.
/// @param y Top edge in logical pixels.
/// @param w Width in logical pixels.
/// @param h Height in logical pixels.
/// @param color Packed RGB runtime color.
/// @param alpha Normalized opacity.
void rt_canvas3d_draw_frame2d(
    void *obj, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, double alpha) {
    int8_t started_temp_frame = 0;
    float r;
    float g;
    float b;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (w <= 0 || h <= 0)
        return;
    if (!isfinite(alpha))
        alpha = 1.0;
    if (alpha < 0.0)
        alpha = 0.0;
    if (alpha > 1.0)
        alpha = 1.0;
    if (alpha <= 0.0001)
        return;
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }
    r = (float)((color >> 16) & 0xFF) / 255.0f;
    g = (float)((color >> 8) & 0xFF) / 255.0f;
    b = (float)(color & 0xFF) / 255.0f;
    (void)canvas3d_queue_screen_rect(c, (float)x, (float)y, (float)w, 1.0f, r, g, b, (float)alpha);
    (void)canvas3d_queue_screen_rect(
        c, (float)x, (float)y + (float)h - 1.0f, (float)w, 1.0f, r, g, b, (float)alpha);
    if (h > 2) {
        (void)canvas3d_queue_screen_rect(
            c, (float)x, (float)y + 1.0f, 1.0f, (float)h - 2.0f, r, g, b, (float)alpha);
        (void)canvas3d_queue_screen_rect(c,
                                         (float)x + (float)w - 1.0f,
                                         (float)y + 1.0f,
                                         1.0f,
                                         (float)h - 2.0f,
                                         r,
                                         g,
                                         b,
                                         (float)alpha);
    }
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw a screen-space filled rounded rectangle with explicit opacity (Plan 08).
/// @param obj Borrowed Canvas3D handle.
/// @param x Left edge in logical pixels.
/// @param y Top edge in logical pixels.
/// @param w Width in logical pixels.
/// @param h Height in logical pixels.
/// @param radius Corner radius in logical pixels.
/// @param color Packed RGB runtime color.
/// @param alpha Normalized opacity.
void rt_canvas3d_draw_round_rect2d(void *obj,
                                   int64_t x,
                                   int64_t y,
                                   int64_t w,
                                   int64_t h,
                                   int64_t radius,
                                   int64_t color,
                                   double alpha) {
    int8_t started_temp_frame = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (w <= 0 || h <= 0)
        return;
    if (!isfinite(alpha))
        alpha = 1.0;
    if (alpha < 0.0)
        alpha = 0.0;
    if (alpha > 1.0)
        alpha = 1.0;
    if (alpha <= 0.0001)
        return;
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }
    (void)canvas3d_queue_screen_round_rect(c,
                                           (float)x,
                                           (float)y,
                                           (float)w,
                                           (float)h,
                                           (float)(radius < 0 ? 0 : radius),
                                           (float)((color >> 16) & 0xFF) / 255.0f,
                                           (float)((color >> 8) & 0xFF) / 255.0f,
                                           (float)(color & 0xFF) / 255.0f,
                                           (float)alpha);
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw a screen-space rounded rectangle outline with explicit opacity (Plan 08).
/// @details Walks the same perimeter as the filled rounded rect (four quarter-arcs,
///          6 segments each, joined by straight edges) with 1px line segments.
/// @param obj Borrowed Canvas3D handle.
/// @param x Left edge in logical pixels.
/// @param y Top edge in logical pixels.
/// @param w Width in logical pixels.
/// @param h Height in logical pixels.
/// @param radius Corner radius in logical pixels.
/// @param color Packed RGB runtime color.
/// @param alpha Normalized opacity.
void rt_canvas3d_draw_round_frame2d(void *obj,
                                    int64_t x,
                                    int64_t y,
                                    int64_t w,
                                    int64_t h,
                                    int64_t radius,
                                    int64_t color,
                                    double alpha) {
    enum { RRF_SEG = 6 };

    int8_t started_temp_frame = 0;
    float rad;
    float half_min;
    float px[4 * (RRF_SEG + 1)];
    float py[4 * (RRF_SEG + 1)];
    int32_t count = 0;
    float r;
    float g;
    float b;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (w <= 0 || h <= 0)
        return;
    if (!isfinite(alpha))
        alpha = 1.0;
    if (alpha < 0.0)
        alpha = 0.0;
    if (alpha > 1.0)
        alpha = 1.0;
    if (alpha <= 0.0001)
        return;
    half_min = (float)(w < h ? w : h) * 0.5f;
    rad = (float)(radius < 0 ? 0 : radius);
    if (rad > half_min)
        rad = half_min;
    if (rad < 0.5f) {
        rt_canvas3d_draw_frame2d(obj, x, y, w, h, color, alpha);
        return;
    }
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }

    {
        const float right = (float)x + (float)w;
        const float bottom = (float)y + (float)h;
        const float ccx[4] = {(float)x + rad, right - rad, right - rad, (float)x + rad};
        const float ccy[4] = {(float)y + rad, (float)y + rad, bottom - rad, bottom - rad};
        const float start_ang[4] = {3.14159265f, 4.71238898f, 0.0f, 1.57079633f};
        for (int corner = 0; corner < 4; corner++) {
            for (int s = 0; s <= RRF_SEG; s++) {
                float ang = start_ang[corner] + 1.57079633f * (float)s / (float)RRF_SEG;
                px[count] = ccx[corner] + cosf(ang) * rad;
                py[count] = ccy[corner] + sinf(ang) * rad;
                count++;
            }
        }
    }
    r = (float)((color >> 16) & 0xFF) / 255.0f;
    g = (float)((color >> 8) & 0xFF) / 255.0f;
    b = (float)(color & 0xFF) / 255.0f;
    for (int32_t i = 0; i < count; i++) {
        int32_t j = (i + 1) % count;
        (void)canvas3d_queue_screen_line(
            c, px[i], py[i], px[j], py[j], 1.0f, r, g, b, (float)alpha);
    }
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw screen-space text scaled by a size multiplier (Plan 08).
/// @param obj Borrowed Canvas3D handle.
/// @param x Left text origin in logical pixels.
/// @param y Top text origin in logical pixels.
/// @param text Borrowed runtime string.
/// @param color Packed RGB runtime color.
/// @param scale Positive bitmap-font scale.
void rt_canvas3d_draw_text2d_scaled(
    void *obj, int64_t x, int64_t y, rt_string text, int64_t color, double scale) {
    int8_t started_temp_frame = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !text)
        return;
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }
    rt_canvas3d_draw_text_3d_scaled(c, x, y, text, color, scale);
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Blit a sub-region of a Pixels image into the overlay (Plan 08).
/// @param obj Borrowed Canvas3D handle.
/// @param x Left destination edge in logical pixels.
/// @param y Top destination edge in logical pixels.
/// @param w Destination width in logical pixels.
/// @param h Destination height in logical pixels.
/// @param pixels Borrowed Pixels source retained through replay.
/// @param sx Source-region left edge in pixels.
/// @param sy Source-region top edge in pixels.
/// @param sw Source-region width in pixels.
/// @param sh Source-region height in pixels.
void rt_canvas3d_draw_image2d_region(void *obj,
                                     int64_t x,
                                     int64_t y,
                                     int64_t w,
                                     int64_t h,
                                     void *pixels,
                                     int64_t sx,
                                     int64_t sy,
                                     int64_t sw,
                                     int64_t sh) {
    int8_t started_temp_frame = 0;
    int64_t pw;
    int64_t ph;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !pixels)
        return;
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0)
        return;
    pw = rt_pixels_width(pixels);
    ph = rt_pixels_height(pixels);
    if (pw <= 0 || ph <= 0)
        return;
    /* Validate without evaluating `sx + sw` / `sy + sh`: caller-controlled signed
     * coordinates must not be able to overflow before conversion to normalized UVs. */
    if (sx < 0 || sy < 0 || sw > pw || sh > ph || sx > pw - sw || sy > ph - sh)
        return;
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }
    (void)canvas3d_queue_screen_image_uv(c,
                                         (float)x,
                                         (float)y,
                                         (float)w,
                                         (float)h,
                                         pixels,
                                         (float)sx / (float)pw,
                                         (float)sy / (float)ph,
                                         ((float)sx + (float)sw) / (float)pw,
                                         ((float)sy + (float)sh) / (float)ph);
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw anti-aliased screen-space text at an arbitrary scale.
/// @details The string is rasterized from the built-in 8x8 bitmap font with a
///   4x4 box filter per output pixel (coverage -> alpha), written into a
///   Pixels, and queued as one image blit. This keeps the chunky pixel *style*
///   while giving clean edges at fractional scales (1.5x, 3.7x, ...). A bounded
///   canvas-owned LRU retains repeated labels across frames; the normal temp-object
///   queue adds a submission-lifetime reference for the current frame.
/// @param obj Borrowed Canvas3D handle.
/// @param x Left text origin in logical pixels.
/// @param y Top text origin in logical pixels.
/// @param text Borrowed runtime string copied or keyed before return.
/// @param color Packed RGB runtime color.
/// @param scale Requested raster scale, sanitized to the supported range.
void rt_canvas3d_draw_text2d_aa(
    void *obj, int64_t x, int64_t y, rt_string text, int64_t color, double scale) {
    int8_t started_temp_frame = 0;
    const char *str;
    size_t len;
    int32_t out_w;
    int32_t out_h;
    void *pixels;
    int cache_owns_pixels = 0;
    int64_t rgb_color;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !text)
        return;
    str = rt_string_cstr(text);
    if (!str)
        return;
    len = strnlen(str, RT_CANVAS3D_TEXT_MAX_BYTES);
    if (len == 0)
        return;
    if (!isfinite(scale) || scale <= 0.0)
        scale = 1.0;
    if (scale > 64.0)
        scale = 64.0;
    out_w = (int32_t)ceil((double)len * 8.0 * scale);
    out_h = (int32_t)ceil(8.0 * scale);
    if (out_w <= 0 || out_h <= 0 || out_w > VGFX3D_RENDERTARGET_DIM_MAX)
        return;
    rgb_color = color & 0xFFFFFF;

    pixels = canvas3d_find_aa_text_cache_entry(c, str, len, rgb_color, scale, out_w, out_h, 0);
    if (pixels) {
        cache_owns_pixels = 1;
    } else {
        pixels = rt_pixels_new((int64_t)out_w, (int64_t)out_h);
        if (!pixels)
            return;
        {
            int64_t rgb_hi = ((rgb_color >> 16) & 0xFF);
            int64_t rgb_mid = ((rgb_color >> 8) & 0xFF);
            int64_t rgb_lo = (rgb_color & 0xFF);
            double inv_scale = 1.0 / scale;
            for (int32_t oy = 0; oy < out_h; oy++) {
                for (int32_t ox = 0; ox < out_w; ox++) {
                    int hits = 0;
                    for (int sy = 0; sy < 4; sy++) {
                        for (int sx = 0; sx < 4; sx++) {
                            double fx = ((double)ox + ((double)sx + 0.5) * 0.25) * inv_scale;
                            double fy = ((double)oy + ((double)sy + 0.5) * 0.25) * inv_scale;
                            int32_t ci = (int32_t)(fx / 8.0);
                            int32_t gx = (int32_t)fx - ci * 8;
                            int32_t gy = (int32_t)fy;
                            if (ci < 0 || (size_t)ci >= len || gx < 0 || gx > 7 || gy < 0 || gy > 7)
                                continue;
                            const uint8_t *glyph = rt_font_get_glyph((int)(unsigned char)str[ci]);
                            if (glyph && (glyph[gy] & (uint8_t)(0x80u >> gx)))
                                hits++;
                        }
                    }
                    if (hits == 0)
                        continue;
                    int64_t alpha = (int64_t)((hits * 255) / 16);
                    int64_t packed = (rgb_hi << 24) | (rgb_mid << 16) | (rgb_lo << 8) | alpha;
                    rt_pixels_set_rgba(pixels, ox, oy, packed);
                }
            }
        }
        cache_owns_pixels = canvas3d_insert_aa_text_cache_entry(
            c, str, len, rgb_color, scale, out_w, out_h, 0, pixels);
    }

    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1)) {
            if (!cache_owns_pixels && rt_obj_release_check0(pixels))
                rt_obj_free(pixels);
            return;
        }
        started_temp_frame = 1;
    }
    if (!rt_canvas3d_add_temp_object(c, pixels)) {
        if (!cache_owns_pixels && rt_obj_release_check0(pixels))
            rt_obj_free(pixels);
        if (started_temp_frame)
            rt_canvas3d_end(c);
        return;
    }
    if (!cache_owns_pixels && rt_obj_release_check0(pixels))
        rt_obj_free(pixels); /* temp queue holds the surviving reference */
    (void)canvas3d_queue_screen_image_uv(
        c, (float)x, (float)y, (float)out_w, (float)out_h, pixels, 0.0f, 0.0f, 1.0f, 1.0f);
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw UTF-8 text with a loaded TrueType face onto the 2D overlay.
/// @details Rasterizes the string once into a Pixels raster (kerned glyph
///          composite at @p size_px), caches it in the AA-text LRU keyed by
///          the font identity, and submits a textured overlay quad. @p y is
///          the TOP of the text box (matching DrawText2DScaled), not the
///          baseline.
/// @param obj Borrowed Canvas3D handle.
/// @param font Borrowed live TtfFont handle.
/// @param x Left destination edge in logical pixels.
/// @param y Top destination edge in logical pixels.
/// @param text Borrowed runtime string (up to 512 bytes are rendered).
/// @param size_px Font pixel size, clamped to the TtfFont range.
/// @param color Packed 0xRRGGBB (alpha comes from glyph coverage).
void rt_canvas3d_draw_text2d_ttf(
    void *obj, void *font, int64_t x, int64_t y, rt_string text, double size_px, int64_t color) {
    int8_t started_temp_frame = 0;
    const char *str;
    size_t len;
    int32_t out_w;
    int32_t out_h;
    void *pixels;
    int cache_owns_pixels = 0;
    int64_t rgb_color;
    int64_t identity;
    struct vg_font *face;
    vg_text_metrics_t text_metrics;
    vg_font_metrics_t font_metrics;
    char render_text[RT_CANVAS3D_TEXT_MAX_BYTES + 1u];
    double raster_width;
    double raster_height;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !text)
        return;
    face = rt_ttf_font_face(font);
    identity = rt_ttf_font_identity(font);
    if (!face || identity == 0) {
        rt_trap("Canvas3D.DrawText2DTtf: invalid font");
        return;
    }
    len = canvas3d_copy_ttf_text_prefix(text, render_text);
    if (len == 0)
        return;
    str = render_text;
    size_px = rt_ttf_font_clamp_size(size_px);

    memset(&text_metrics, 0, sizeof(text_metrics));
    memset(&font_metrics, 0, sizeof(font_metrics));
    vg_font_measure_text((vg_font_t *)face, (float)size_px, str, &text_metrics);
    vg_font_get_metrics((vg_font_t *)face, (float)size_px, &font_metrics);
    raster_width = ceil((double)text_metrics.width) + 2.0;
    raster_height =
        font_metrics.line_height > 0 ? (double)font_metrics.line_height : ceil(size_px * 1.25);
    if (!isfinite(raster_width) || !isfinite(raster_height) || raster_width <= 2.0 ||
        raster_height <= 0.0 || raster_width > (double)VGFX3D_RENDERTARGET_DIM_MAX ||
        raster_height > (double)VGFX3D_RENDERTARGET_DIM_MAX)
        return;
    out_w = (int32_t)raster_width;
    out_h = (int32_t)raster_height;
    rgb_color = color & 0xFFFFFF;

    pixels =
        canvas3d_find_aa_text_cache_entry(c, str, len, rgb_color, size_px, out_w, out_h, identity);
    if (pixels) {
        cache_owns_pixels = 1;
    } else {
        pixels = rt_pixels_new((int64_t)out_w, (int64_t)out_h);
        if (!pixels)
            return;
        {
            int64_t rgb_hi = ((rgb_color >> 16) & 0xFF);
            int64_t rgb_mid = ((rgb_color >> 8) & 0xFF);
            int64_t rgb_lo = (rgb_color & 0xFF);
            const char *cursor = str;
            const char *end = str + len;
            double pen = 0.0;
            uint32_t prev_cp = 0;
            while (cursor < end) {
                uint32_t cp = vg_utf8_decode(&cursor);
                const vg_glyph_t *glyph;
                if (cp == 0)
                    break;
                if (prev_cp != 0)
                    pen +=
                        (double)vg_font_get_kerning((vg_font_t *)face, (float)size_px, prev_cp, cp);
                glyph = vg_font_get_glyph((vg_font_t *)face, (float)size_px, cp);
                if (glyph) {
                    int32_t base_x = (int32_t)floor(pen) + glyph->bearing_x;
                    int32_t base_y = font_metrics.ascent - glyph->bearing_y;
                    for (int32_t gy = 0; gy < glyph->height; gy++) {
                        int32_t py = base_y + gy;
                        if (py < 0 || py >= out_h)
                            continue;
                        for (int32_t gx = 0; gx < glyph->width; gx++) {
                            int32_t px = base_x + gx;
                            uint8_t alpha;
                            if (px < 0 || px >= out_w)
                                continue;
                            alpha = glyph->bitmap[(size_t)gy * (size_t)glyph->width + (size_t)gx];
                            if (alpha == 0)
                                continue;
                            {
                                /* Kerned glyphs may overlap: keep the higher
                                 * coverage instead of overwriting. */
                                int64_t existing =
                                    rt_pixels_get_rgba(pixels, (int64_t)px, (int64_t)py);
                                int64_t existing_alpha = existing & 0xFF;
                                if ((int64_t)alpha > existing_alpha) {
                                    int64_t packed = (rgb_hi << 24) | (rgb_mid << 16) |
                                                     (rgb_lo << 8) | (int64_t)alpha;
                                    rt_pixels_set_rgba(pixels, (int64_t)px, (int64_t)py, packed);
                                }
                            }
                        }
                    }
                    pen += (double)glyph->advance;
                }
                prev_cp = cp;
            }
        }
        cache_owns_pixels = canvas3d_insert_aa_text_cache_entry(
            c, str, len, rgb_color, size_px, out_w, out_h, identity, pixels);
    }

    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1)) {
            if (!cache_owns_pixels && rt_obj_release_check0(pixels))
                rt_obj_free(pixels);
            return;
        }
        started_temp_frame = 1;
    }
    if (!rt_canvas3d_add_temp_object(c, pixels)) {
        if (!cache_owns_pixels && rt_obj_release_check0(pixels))
            rt_obj_free(pixels);
        if (started_temp_frame)
            rt_canvas3d_end(c);
        return;
    }
    if (!cache_owns_pixels && rt_obj_release_check0(pixels))
        rt_obj_free(pixels); /* temp queue holds the surviving reference */
    (void)canvas3d_queue_screen_image_uv(
        c, (float)x, (float)y, (float)out_w, (float)out_h, pixels, 0.0f, 0.0f, 1.0f, 1.0f);
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Width in pixels of DrawText2DTtf output for @p text at @p size_px.
/// @param obj Borrowed Canvas3D handle used for validation.
/// @param font Borrowed live TtfFont handle.
/// @param text Borrowed runtime string measured up to the renderer limit.
/// @param size_px Font pixel size, clamped to the TtfFont range.
/// @return Ceil-rounded output width in logical pixels, or zero for invalid
///         input.
int64_t rt_canvas3d_measure_text2d_ttf(void *obj, void *font, rt_string text, double size_px) {
    vg_text_metrics_t metrics;
    struct vg_font *face;
    char render_text[RT_CANVAS3D_TEXT_MAX_BYTES + 1u];
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return 0;
    face = rt_ttf_font_face(font);
    if (!face) {
        rt_trap("Canvas3D.MeasureText2DTtf: invalid font");
        return 0;
    }
    if (canvas3d_copy_ttf_text_prefix(text, render_text) == 0)
        return 0;
    memset(&metrics, 0, sizeof(metrics));
    vg_font_measure_text(
        (vg_font_t *)face, (float)rt_ttf_font_clamp_size(size_px), render_text, &metrics);
    if (!isfinite(metrics.width) || metrics.width <= 0.0f || metrics.width >= (float)INT64_MAX)
        return 0;
    return (int64_t)ceil((double)metrics.width);
}

/// @brief Width in pixels of DrawText2DAA output for @p text at @p scale.
/// @param obj Borrowed Canvas3D handle used for validation.
/// @param text Borrowed runtime string measured up to the AA renderer's length limit.
/// @param scale Requested raster scale, sanitized to the supported range.
/// @return Ceil-rounded output width in logical pixels, or zero for invalid input.
int64_t rt_canvas3d_measure_text2d_aa(void *obj, rt_string text, double scale) {
    const char *str;
    size_t len;
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !text)
        return 0;
    str = rt_string_cstr(text);
    if (!str)
        return 0;
    len = strnlen(str, RT_CANVAS3D_TEXT_MAX_BYTES);
    if (!isfinite(scale) || scale <= 0.0)
        scale = 1.0;
    if (scale > 64.0)
        scale = 64.0;
    return (int64_t)ceil((double)len * 8.0 * scale);
}

/// @brief Draw a 9-slice image: corners unscaled, edges stretched on one axis,
///   center stretched on both — HUD panels/buttons from a single texture.
/// @param obj Borrowed Canvas3D handle.
/// @param x Left destination edge in logical pixels.
/// @param y Top destination edge in logical pixels.
/// @param w Destination width in logical pixels.
/// @param h Destination height in logical pixels.
/// @param pixels Borrowed Pixels source retained through replay.
/// @param inset_l Left source inset in pixels.
/// @param inset_t Top source inset in pixels.
/// @param inset_r Right source inset in pixels.
/// @param inset_b Bottom source inset in pixels.
void rt_canvas3d_draw_image2d_nine_slice(void *obj,
                                         int64_t x,
                                         int64_t y,
                                         int64_t w,
                                         int64_t h,
                                         void *pixels,
                                         int64_t inset_l,
                                         int64_t inset_t,
                                         int64_t inset_r,
                                         int64_t inset_b) {
    int8_t started_temp_frame = 0;
    int64_t pw;
    int64_t ph;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !pixels || w <= 0 || h <= 0)
        return;
    pw = rt_pixels_width(pixels);
    ph = rt_pixels_height(pixels);
    if (pw <= 0 || ph <= 0)
        return;
    if (inset_l < 0)
        inset_l = 0;
    if (inset_t < 0)
        inset_t = 0;
    if (inset_r < 0)
        inset_r = 0;
    if (inset_b < 0)
        inset_b = 0;
    if (inset_l >= pw || inset_r >= pw - inset_l) {
        inset_l = pw / 3;
        inset_r = pw / 3;
    }
    if (inset_t >= ph || inset_b >= ph - inset_t) {
        inset_t = ph / 3;
        inset_b = ph / 3;
    }
    /* Destination insets clamp to half the rect so slices never overlap. */
    int64_t dl = inset_l;
    int64_t dr = inset_r;
    int64_t dt = inset_t;
    int64_t db = inset_b;
    if (dl > w || dr > w - dl) {
        dl = w / 2;
        dr = w - dl;
    }
    if (dt > h || db > h - dt) {
        dt = h / 2;
        db = h - dt;
    }
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }
    {
        const float su[4] = {
            0.0f, (float)inset_l / (float)pw, (float)(pw - inset_r) / (float)pw, 1.0f};
        const float sv[4] = {
            0.0f, (float)inset_t / (float)ph, (float)(ph - inset_b) / (float)ph, 1.0f};
        const float dx[4] = {
            (float)x, (float)x + (float)dl, (float)x + ((float)w - (float)dr), (float)x + (float)w};
        const float dy[4] = {
            (float)y, (float)y + (float)dt, (float)y + ((float)h - (float)db), (float)y + (float)h};
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                float dw = dx[col + 1] - dx[col];
                float dh = dy[row + 1] - dy[row];
                if (dw <= 0.0f || dh <= 0.0f)
                    continue;
                (void)canvas3d_queue_screen_image_uv(c,
                                                     dx[col],
                                                     dy[row],
                                                     dw,
                                                     dh,
                                                     pixels,
                                                     su[col],
                                                     sv[row],
                                                     su[col + 1],
                                                     sv[row + 1]);
            }
        }
    }
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Restrict subsequent overlay 2D drawing to a screen rect (Plan 08).
/// @details Enqueue-time CPU clipping: rects, lines, images, and text queued while the
///          clip is active are trimmed canvas-side, so all four backends behave
///          identically. Degenerate rects clear the clip.
/// @param obj Borrowed Canvas3D handle.
/// @param x Left clip edge in logical pixels.
/// @param y Top clip edge in logical pixels.
/// @param w Clip width; non-positive values clear clipping.
/// @param h Clip height; non-positive values clear clipping.
void rt_canvas3d_set_clip_rect2d(void *obj, int64_t x, int64_t y, int64_t w, int64_t h) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (w <= 0 || h <= 0) {
        c->overlay_clip_active = 0;
        return;
    }
    c->overlay_clip_active = 1;
    c->overlay_clip_x = (float)x;
    c->overlay_clip_y = (float)y;
    c->overlay_clip_w = (float)w;
    c->overlay_clip_h = (float)h;
}

/// @brief Remove the overlay 2D clip rect (Plan 08).
/// @param obj Borrowed Canvas3D handle.
void rt_canvas3d_clear_clip_rect2d(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->overlay_clip_active = 0;
}

/// @brief `Canvas3D.ClipRectActive` — whether an overlay clip rect is set (ADR 0233).
/// @param obj Borrowed Canvas3D handle.
/// @return Nonzero while a clip rect is active.
int8_t rt_canvas3d_get_clip_rect_active(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->overlay_clip_active ? 1 : 0;
}

/// @brief `Canvas3D.ClipRectX` — retained clip-rect left edge (ADR 0233).
/// @param obj Borrowed Canvas3D handle.
/// @return Retained left edge in logical pixels; zero when no clip is active.
int64_t rt_canvas3d_get_clip_rect_x(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->overlay_clip_active ? canvas3d_overlay_float_to_i64(c->overlay_clip_x) : 0;
}

/// @brief `Canvas3D.ClipRectY` — retained clip-rect top edge (ADR 0233).
/// @param obj Borrowed Canvas3D handle.
/// @return Retained top edge in logical pixels; zero when no clip is active.
int64_t rt_canvas3d_get_clip_rect_y(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->overlay_clip_active ? canvas3d_overlay_float_to_i64(c->overlay_clip_y) : 0;
}

/// @brief `Canvas3D.ClipRectWidth` — retained clip-rect width (ADR 0233).
/// @param obj Borrowed Canvas3D handle.
/// @return Retained width in logical pixels; zero when no clip is active.
int64_t rt_canvas3d_get_clip_rect_width(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->overlay_clip_active ? canvas3d_overlay_float_to_i64(c->overlay_clip_w) : 0;
}

/// @brief `Canvas3D.ClipRectHeight` — retained clip-rect height (ADR 0233).
/// @param obj Borrowed Canvas3D handle.
/// @return Retained height in logical pixels; zero when no clip is active.
int64_t rt_canvas3d_get_clip_rect_height(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->overlay_clip_active ? canvas3d_overlay_float_to_i64(c->overlay_clip_h) : 0;
}

/// @brief Width in pixels of DrawText2DScaled output for @p text at @p scale (Plan 08).
/// @details The built-in font advances 6 dots per character at 2px per dot.
/// @param obj Borrowed Canvas3D handle used for validation.
/// @param text Borrowed runtime string.
/// @param scale Requested size multiplier, sanitized to the supported range.
/// @return Logical text advance in pixels, or zero for invalid input.
int64_t rt_canvas3d_measure_text2d(void *obj, rt_string text, double scale) {
    const char *str;
    size_t len = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !text)
        return 0;
    str = rt_string_cstr(text);
    if (!str)
        return 0;
    len = strnlen(str, RT_CANVAS3D_TEXT_MAX_BYTES);
    if (!isfinite(scale) || scale <= 0.0)
        scale = 1.0;
    if (scale > 64.0)
        scale = 64.0;
    return (int64_t)((double)len * 6.0 * 2.0 * scale);
}

/// @brief Draw a centered crosshair (FPS reticle) at screen center with `size` arms in `color`.
/// @param obj Borrowed Canvas3D handle.
/// @param color Packed RGB runtime color.
/// @param size Total arm span in logical pixels.
void rt_canvas3d_draw_crosshair(void *obj, int64_t color, int64_t size) {
    int8_t started_temp_frame = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    int32_t out_w = 0;
    int32_t out_h = 0;
    if (!overlay_output_size(c, &out_w, &out_h))
        return;

    {
        const int32_t cx = out_w / 2;
        const int32_t cy = out_h / 2;
        int64_t max_span = (int64_t)(out_w > out_h ? out_w : out_h) * 2;
        float half;
        const float r = (float)((color >> 16) & 0xFF) / 255.0f;
        const float g = (float)((color >> 8) & 0xFF) / 255.0f;
        const float b = (float)(color & 0xFF) / 255.0f;

        if (size <= 0)
            size = 1;
        if (size > max_span)
            size = max_span;
        half = (float)size * 0.5f;
        if (!c->in_frame) {
            if (!canvas3d_begin_overlay_frame(c, 1))
                return;
            started_temp_frame = 1;
        }
        (void)canvas3d_queue_screen_line(
            c, (float)cx - half, (float)cy, (float)cx + half, (float)cy, 1.0f, r, g, b, 1.0f);
        (void)canvas3d_queue_screen_line(
            c, (float)cx, (float)cy - half, (float)cx, (float)cy + half, 1.0f, r, g, b, 1.0f);
    }
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Draw screen-space text at (x, y) using the built-in 8×8 font in `color`.
/// @param obj Borrowed Canvas3D handle.
/// @param x Left text origin in logical pixels.
/// @param y Top text origin in logical pixels.
/// @param text Borrowed runtime string.
/// @param color Packed RGB runtime color.
void rt_canvas3d_draw_text2d(void *obj, int64_t x, int64_t y, rt_string text, int64_t color) {
    int8_t started_temp_frame = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !text)
        return;
    if (!c->in_frame) {
        if (!canvas3d_begin_overlay_frame(c, 1))
            return;
        started_temp_frame = 1;
    }
    rt_canvas3d_draw_text_3d(c, x, y, text, color);
    if (started_temp_frame)
        rt_canvas3d_end(c);
}

/// @brief Return the active backend name as a string ("metal", "d3d11", "opengl", ...).
/// Useful for backend-specific debug output / feature gating.
/// @param obj Borrowed Canvas3D handle.
/// @return New runtime string reference naming the active backend, or `"unknown"`.
rt_string rt_canvas3d_get_backend(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return rt_const_cstr("unknown");
    return rt_const_cstr(c->backend ? c->backend->name : "unknown");
}

/// @brief Return whether the active canvas fell back from a selected GPU backend to software.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-zero when backend creation used a fallback.
int8_t rt_canvas3d_get_backend_fallback(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return (c && c->backend_fallback) ? 1 : 0;
}

/// @brief Return the reason Canvas3D fell back to software, or an empty string.
/// @details The returned string is a static runtime constant, not per-call heap storage. It lets
///          tools distinguish an unavailable selected backend from one that failed to initialize
///          without scraping stderr.
/// @param obj Borrowed Canvas3D handle.
/// @return New runtime string reference containing the fallback reason, or an empty string.
rt_string rt_canvas3d_get_backend_fallback_reason(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !c->backend_fallback || !c->backend_fallback_reason)
        return rt_const_cstr("");
    return rt_const_cstr(c->backend_fallback_reason);
}

/// @brief True when the active backend has the GPU many-light shader/upload path.
/// @details Driven by the vtable's `clustered_lighting` field so a new backend
///          opts in by declaration instead of editing this file. Fake unit-test
///          backends stay at 0 regardless of a GPU-like name; backends named
///          "software" keep the CPU-parity classification used throughout this
///          file (covers stack/mock software canvases with zeroed fields).
/// @param backend Borrowed backend vtable to classify.
/// @return Non-zero for production backends with the many-light implementation.
static int canvas3d_backend_supports_clustered_lighting(const vgfx3d_backend_t *backend) {
    if (!backend)
        return 0;
    if (backend == &vgfx3d_software_backend ||
        (backend->name && strcmp(backend->name, "software") == 0))
        return 1;
    return backend->clustered_lighting != 0;
}

/// @brief True when the backend can consume multiple shadow slots as primary-light cascades.
/// @details Requires both the declared `shadow_csm` vtable field and the live
///          shadow hooks; "software"-named backends keep the CPU-parity path.
/// @param backend Borrowed backend vtable to classify.
/// @return Non-zero when required shadow hooks and a production CSM path exist.
static int canvas3d_backend_supports_shadow_csm(const vgfx3d_backend_t *backend) {
    if (!backend || !backend->shadow_begin || !backend->shadow_draw || !backend->shadow_end)
        return 0;
    if (backend == &vgfx3d_software_backend ||
        (backend->name && strcmp(backend->name, "software") == 0))
        return 1;
    return backend->shadow_csm != 0;
}

/// @brief Return whether @p backend can submit ordinary 3D draw commands.
///
/// @details Several Canvas3D feature bits describe material, animation, scene, and CPU culling
///          behavior layered around a backend draw path. Partial diagnostic/test backends can
///          expose telemetry hooks without being drawable, so those feature bits are advertised
///          only when the base submit hook exists.
/// @param backend Borrowed backend vtable to inspect.
/// @return Non-zero when ordinary mesh submission is available.
static int canvas3d_backend_has_draw_path(const vgfx3d_backend_t *backend) {
    return backend && backend->submit_draw;
}

/// @brief Return the feature bits advertised by the active backend.
/// @details The mask is based on backend vtable hooks plus the software
///          fallback paths owned by Canvas3D. This lets applications choose
///          production-safe rendering paths without hardcoding backend names.
/// @param obj Borrowed Canvas3D handle.
/// @return Bitwise OR of truthful RT_CANVAS3D_BACKEND_CAP_* flags, or zero.
int64_t rt_canvas3d_get_backend_capabilities(void *obj) {
    int64_t caps = 0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return 0;
    const vgfx3d_backend_t *backend = c->backend;
    int draw_path;
    if (!backend)
        return 0;
    draw_path = canvas3d_backend_has_draw_path(backend);

    if (backend == &vgfx3d_software_backend ||
        (backend->name && strcmp(backend->name, "software") == 0))
        caps |= RT_CANVAS3D_BACKEND_CAP_SOFTWARE;
    else
        caps |= RT_CANVAS3D_BACKEND_CAP_GPU;

    if (backend->set_render_target)
        caps |= RT_CANVAS3D_BACKEND_CAP_RENDER_TARGET;
    if (backend->readback_rgba || (caps & RT_CANVAS3D_BACKEND_CAP_SOFTWARE))
        caps |= RT_CANVAS3D_BACKEND_CAP_WINDOW_READBACK;
    if (backend->shadow_begin && backend->shadow_draw && backend->shadow_end)
        caps |= RT_CANVAS3D_BACKEND_CAP_SHADOWS;
    if (backend->draw_skybox || (caps & RT_CANVAS3D_BACKEND_CAP_SOFTWARE))
        caps |= RT_CANVAS3D_BACKEND_CAP_SKYBOX;
    if (backend->submit_draw_instanced)
        caps |= RT_CANVAS3D_BACKEND_CAP_INSTANCING;
    if (backend->submit_draw_instanced && (caps & RT_CANVAS3D_BACKEND_CAP_GPU))
        caps |= RT_CANVAS3D_BACKEND_CAP_HARDWARE_INSTANCING;
    if (draw_path && backend->shadow_atlas_slots && backend->shadow_begin && backend->shadow_draw &&
        backend->shadow_end)
        caps |= RT_CANVAS3D_BACKEND_CAP_SHADOW_POINT;
    /* Depth-aware post-FX runs everywhere: natively on GPU backends, via the
     * CPU parity implementations on software. */
    if (backend->present_postfx || (draw_path && (caps & RT_CANVAS3D_BACKEND_CAP_SOFTWARE)))
        caps |= RT_CANVAS3D_BACKEND_CAP_POSTFX_FULL;
    if (backend->present_postfx || (draw_path && (caps & RT_CANVAS3D_BACKEND_CAP_SOFTWARE)))
        caps |= RT_CANVAS3D_BACKEND_CAP_POSTFX;
    if (backend->present_postfx)
        caps |= RT_CANVAS3D_BACKEND_CAP_GPU_POSTFX;
    /* Device-specific feature bits such as HDR scene color/TAA depend on the
     * concrete context, not merely the presence of a post-FX vtable hook. */
    if (backend->get_feature_caps)
        caps |= backend->get_feature_caps(c->backend_ctx);
    if (draw_path && (caps & RT_CANVAS3D_BACKEND_CAP_SOFTWARE))
        caps |= RT_CANVAS3D_BACKEND_CAP_POSTFX_OVERLAY;
    if (backend->present_postfx && backend->apply_postfx && backend->present)
        caps |= RT_CANVAS3D_BACKEND_CAP_POSTFX_OVERLAY | RT_CANVAS3D_BACKEND_CAP_GPU_POSTFX_OVERLAY;
    if (caps & RT_CANVAS3D_BACKEND_CAP_WINDOW_READBACK)
        caps |= RT_CANVAS3D_BACKEND_CAP_FINAL_SCREENSHOT;
    if (draw_path && canvas3d_backend_supports_clustered_lighting(backend))
        caps |= RT_CANVAS3D_BACKEND_CAP_CLUSTERED_LIGHTING;
    /* Plan 10: soft particles need the opaque->transparent depth snapshot hook. */
    if (draw_path && backend->resolve_opaque_targets)
        caps |= RT_CANVAS3D_BACKEND_CAP_SOFT_PARTICLES;
    /* Plan 10: the SSR post pass rides the GPU postfx pipeline. */
    if (draw_path && backend->present_postfx)
        caps |= RT_CANVAS3D_BACKEND_CAP_SSR;
    if (draw_path && canvas3d_backend_supports_shadow_csm(backend))
        caps |= RT_CANVAS3D_BACKEND_CAP_SHADOW_CSM;
    if (backend->get_native_texture_caps)
        caps |= backend->get_native_texture_caps(c->backend_ctx) &
                (RT_CANVAS3D_BACKEND_CAP_BC7 | RT_CANVAS3D_BACKEND_CAP_ASTC |
                 RT_CANVAS3D_BACKEND_CAP_ETC2 | RT_CANVAS3D_BACKEND_CAP_ANISOTROPY |
                 RT_CANVAS3D_BACKEND_CAP_BC1 | RT_CANVAS3D_BACKEND_CAP_BC3 |
                 RT_CANVAS3D_BACKEND_CAP_BC4 | RT_CANVAS3D_BACKEND_CAP_BC5);
    if (draw_path) {
        caps |= RT_CANVAS3D_BACKEND_CAP_PBR | RT_CANVAS3D_BACKEND_CAP_NORMAL_MAPS |
                RT_CANVAS3D_BACKEND_CAP_ALPHA_MASK | RT_CANVAS3D_BACKEND_CAP_MORPH_TARGETS |
                RT_CANVAS3D_BACKEND_CAP_SKINNING | RT_CANVAS3D_BACKEND_CAP_TERRAIN_SPLAT;
        caps |= RT_CANVAS3D_BACKEND_CAP_OCCLUSION | RT_CANVAS3D_BACKEND_CAP_HLOD;
    }

    return caps;
}

/// @brief Convert a user-facing capability string to its internal bitmask flag.
/// @details `Canvas3D.SupportsCapability("shadows")` flows through here so the Zia-side
///   name survives as a readable string rather than a numeric enum. Several common
///   aliases are accepted per flag ("shadows" / "shadow_maps", "postfx" / "post_fx", etc.)
///   so scripts can use whichever reads more natural. Unknown names return 0, which the
///   caller treats as "capability not supported".
/// @param name Borrowed non-empty NUL-terminated capability name.
/// @return Matching single RT_CANVAS3D_BACKEND_CAP_* bit, or zero when unknown.
static int64_t canvas3d_capability_from_name(const char *name) {
    if (!name || !*name)
        return 0;
    if (strcmp(name, "software") == 0)
        return RT_CANVAS3D_BACKEND_CAP_SOFTWARE;
    if (strcmp(name, "gpu") == 0)
        return RT_CANVAS3D_BACKEND_CAP_GPU;
    if (strcmp(name, "render_target") == 0 || strcmp(name, "rendertarget") == 0)
        return RT_CANVAS3D_BACKEND_CAP_RENDER_TARGET;
    if (strcmp(name, "window_readback") == 0 || strcmp(name, "readback") == 0 ||
        strcmp(name, "screenshot") == 0)
        return RT_CANVAS3D_BACKEND_CAP_WINDOW_READBACK;
    if (strcmp(name, "shadows") == 0 || strcmp(name, "shadow_maps") == 0)
        return RT_CANVAS3D_BACKEND_CAP_SHADOWS;
    if (strcmp(name, "skybox") == 0 || strcmp(name, "cubemap_skybox") == 0)
        return RT_CANVAS3D_BACKEND_CAP_SKYBOX;
    if (strcmp(name, "hardware_instancing") == 0)
        return RT_CANVAS3D_BACKEND_CAP_HARDWARE_INSTANCING;
    if (strcmp(name, "instancing") == 0)
        return RT_CANVAS3D_BACKEND_CAP_INSTANCING;
    if (strcmp(name, "shadow-point") == 0 || strcmp(name, "shadow_point") == 0 ||
        strcmp(name, "point-shadows") == 0)
        return RT_CANVAS3D_BACKEND_CAP_SHADOW_POINT;
    if (strcmp(name, "postfx-full") == 0 || strcmp(name, "postfx_full") == 0)
        return RT_CANVAS3D_BACKEND_CAP_POSTFX_FULL;
    if (strcmp(name, "postfx") == 0 || strcmp(name, "post_fx") == 0 || strcmp(name, "bloom") == 0 ||
        strcmp(name, "tonemap") == 0 || strcmp(name, "tone_map") == 0 ||
        strcmp(name, "color-grade") == 0 || strcmp(name, "color_grade") == 0 ||
        strcmp(name, "colorgrade") == 0 || strcmp(name, "vignette") == 0 ||
        strcmp(name, "fxaa") == 0)
        return RT_CANVAS3D_BACKEND_CAP_POSTFX;
    /* SSAO / depth-of-field / motion blur are GPU-only screen-space passes; alias
     * their effect names to the GPU post-FX capability so a query like
     * BackendSupports("ssao") resolves instead of silently returning false. */
    if (strcmp(name, "gpu_postfx") == 0 || strcmp(name, "gpu_post_fx") == 0 ||
        strcmp(name, "ssao") == 0 || strcmp(name, "dof") == 0 ||
        strcmp(name, "depth-of-field") == 0 || strcmp(name, "depth_of_field") == 0 ||
        strcmp(name, "motion-blur") == 0 || strcmp(name, "motion_blur") == 0 ||
        strcmp(name, "motionblur") == 0)
        return RT_CANVAS3D_BACKEND_CAP_GPU_POSTFX;
    if (strcmp(name, "postfx-overlay") == 0 || strcmp(name, "postfx_overlay") == 0 ||
        strcmp(name, "post_fx_overlay") == 0)
        return RT_CANVAS3D_BACKEND_CAP_POSTFX_OVERLAY;
    if (strcmp(name, "final-screenshot") == 0 || strcmp(name, "final_screenshot") == 0)
        return RT_CANVAS3D_BACKEND_CAP_FINAL_SCREENSHOT;
    if (strcmp(name, "gpu-postfx-overlay") == 0 || strcmp(name, "gpu_postfx_overlay") == 0 ||
        strcmp(name, "gpu_post_fx_overlay") == 0)
        return RT_CANVAS3D_BACKEND_CAP_GPU_POSTFX_OVERLAY;
    if (strcmp(name, "clustered-lighting") == 0 || strcmp(name, "clustered_lighting") == 0 ||
        strcmp(name, "forward_plus") == 0 || strcmp(name, "forward+") == 0)
        return RT_CANVAS3D_BACKEND_CAP_CLUSTERED_LIGHTING;
    if (strcmp(name, "shadow-csm") == 0 || strcmp(name, "shadow_csm") == 0 ||
        strcmp(name, "cascaded-shadows") == 0 || strcmp(name, "cascaded_shadows") == 0)
        return RT_CANVAS3D_BACKEND_CAP_SHADOW_CSM;
    if (strcmp(name, "occlusion") == 0 || strcmp(name, "occlusion-culling") == 0 ||
        strcmp(name, "occlusion_culling") == 0)
        return RT_CANVAS3D_BACKEND_CAP_OCCLUSION;
    if (strcmp(name, "hlod") == 0 || strcmp(name, "impostor") == 0 ||
        strcmp(name, "impostors") == 0 || strcmp(name, "auto-lod") == 0 ||
        strcmp(name, "auto_lod") == 0)
        return RT_CANVAS3D_BACKEND_CAP_HLOD;
    if (strcmp(name, "bc1") == 0)
        return RT_CANVAS3D_BACKEND_CAP_BC1;
    if (strcmp(name, "bc3") == 0)
        return RT_CANVAS3D_BACKEND_CAP_BC3;
    if (strcmp(name, "bc4") == 0)
        return RT_CANVAS3D_BACKEND_CAP_BC4;
    if (strcmp(name, "bc5") == 0)
        return RT_CANVAS3D_BACKEND_CAP_BC5;
    if (strcmp(name, "bc7") == 0)
        return RT_CANVAS3D_BACKEND_CAP_BC7;
    if (strcmp(name, "astc") == 0)
        return RT_CANVAS3D_BACKEND_CAP_ASTC;
    if (strcmp(name, "etc2") == 0)
        return RT_CANVAS3D_BACKEND_CAP_ETC2;
    if (strcmp(name, "anisotropy") == 0 || strcmp(name, "anisotropic-filtering") == 0 ||
        strcmp(name, "anisotropic_filtering") == 0)
        return RT_CANVAS3D_BACKEND_CAP_ANISOTROPY;
    if (strcmp(name, "pbr") == 0 || strcmp(name, "physically-based") == 0 ||
        strcmp(name, "physically_based") == 0)
        return RT_CANVAS3D_BACKEND_CAP_PBR;
    if (strcmp(name, "normal-maps") == 0 || strcmp(name, "normal_maps") == 0 ||
        strcmp(name, "normalmap") == 0)
        return RT_CANVAS3D_BACKEND_CAP_NORMAL_MAPS;
    if (strcmp(name, "alpha-mask") == 0 || strcmp(name, "alpha_mask") == 0 ||
        strcmp(name, "masked-alpha") == 0 || strcmp(name, "masked_alpha") == 0)
        return RT_CANVAS3D_BACKEND_CAP_ALPHA_MASK;
    if (strcmp(name, "morph-targets") == 0 || strcmp(name, "morph_targets") == 0 ||
        strcmp(name, "morphing") == 0)
        return RT_CANVAS3D_BACKEND_CAP_MORPH_TARGETS;
    if (strcmp(name, "skinning") == 0 || strcmp(name, "skeletal-animation") == 0 ||
        strcmp(name, "skeletal_animation") == 0)
        return RT_CANVAS3D_BACKEND_CAP_SKINNING;
    if (strcmp(name, "terrain-splat") == 0 || strcmp(name, "terrain_splat") == 0 ||
        strcmp(name, "terrain-splatting") == 0 || strcmp(name, "terrain_splatting") == 0)
        return RT_CANVAS3D_BACKEND_CAP_TERRAIN_SPLAT;
    if (strcmp(name, "hdr-scene") == 0 || strcmp(name, "hdr_scene") == 0)
        return RT_CANVAS3D_BACKEND_CAP_HDR_SCENE;
    if (strcmp(name, "taa") == 0 || strcmp(name, "temporal-aa") == 0 ||
        strcmp(name, "temporal_aa") == 0)
        return RT_CANVAS3D_BACKEND_CAP_TAA;
    if (strcmp(name, "soft-particles") == 0 || strcmp(name, "soft_particles") == 0)
        return RT_CANVAS3D_BACKEND_CAP_SOFT_PARTICLES;
    if (strcmp(name, "ssr") == 0 || strcmp(name, "screen-space-reflections") == 0 ||
        strcmp(name, "screen_space_reflections") == 0)
        return RT_CANVAS3D_BACKEND_CAP_SSR;
    return 0;
}

/// @brief Convert explicit native compressed-texture capability names to backend bits.
/// @details `texture:*` names intentionally report CPU decode/fallback support for asset loading.
/// This helper backs the less ambiguous `native-texture:*` and `backend-texture:*` names, which
/// report whether the active backend/device can upload and sample that compressed format directly.
/// @param name User-facing capability string.
/// @return Matching RT_CANVAS3D_BACKEND_CAP_* bit, or 0 when @p name is not a recognized native
/// texture capability.
static int64_t canvas3d_native_texture_capability_from_name(const char *name) {
    const char *suffix = NULL;
    if (!name)
        return 0;
    if (strncmp(name, "native-texture:", 15) == 0)
        suffix = name + 15;
    else if (strncmp(name, "native_texture:", 15) == 0)
        suffix = name + 15;
    else if (strncmp(name, "backend-texture:", 16) == 0)
        suffix = name + 16;
    else if (strncmp(name, "backend_texture:", 16) == 0)
        suffix = name + 16;
    if (!suffix || !*suffix)
        return 0;
    return canvas3d_capability_from_name(suffix);
}

/// @brief Return a CPU texture fallback support answer for `texture:*` capability keys.
/// @param name Borrowed NUL-terminated capability name.
/// @return 0/1 for recognized texture keys, -1 when @p name is not a texture capability key.
static int canvas3d_texture_capability_from_name(const char *name) {
    const char *format_name;

    if (!name)
        return -1;
    if (strcmp(name, "texture:ktx2-cpu") == 0 || strcmp(name, "texture:ktx2_cpu") == 0)
        return rt_textureasset3d_cpu_supports_ktx2() ? 1 : 0;
    if (strncmp(name, "texture:", 8) == 0) {
        format_name = name + 8;
        return *format_name && rt_textureasset3d_cpu_supports_format(format_name) ? 1 : 0;
    }
    return -1;
}

/// @brief Return whether the active backend supports a named capability.
/// @param obj Borrowed Canvas3D handle.
/// @param capability Borrowed runtime string using a documented capability name or alias.
/// @return Non-zero only when the active backend/runtime path truthfully supports the request.
int8_t rt_canvas3d_backend_supports(void *obj, rt_string capability) {
    int64_t flag;
    int64_t native_texture_flag;
    int texture_capability;
    const char *name;

    if (!obj || !capability)
        return 0;
    name = rt_string_cstr(capability);
    if (!name)
        return 0;
    if (strcmp(name, "runtime-fallback") == 0 || strcmp(name, "runtime_fallback") == 0 ||
        strcmp(name, "backend-fallback") == 0 || strcmp(name, "backend_fallback") == 0 ||
        strcmp(name, "software-fallback") == 0 || strcmp(name, "software_fallback") == 0)
        return rt_canvas3d_get_backend_fallback(obj);
    if (strcmp(name, "vsync-control") == 0 || strcmp(name, "vsync_control") == 0) {
        rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
        return (c && c->backend && c->backend->set_vsync) ? 1 : 0;
    }
    if (strcmp(name, "render-scale") == 0 || strcmp(name, "render_scale") == 0) {
        rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
        return (c && c->backend && c->backend->set_render_scale) ? 1 : 0;
    }
    if (strcmp(name, "gpu-skinning") == 0 || strcmp(name, "gpu_skinning") == 0) {
        /* GPU backends consume bone palettes in the vertex shader; software stays
         * the CPU-skinned reference. ForceCpuSkinning reports the override too so
         * capability checks match actual routing. */
        rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
        if (!c || !c->backend || !c->backend->name || c->force_cpu_skinning)
            return 0;
        return (strcmp(c->backend->name, "metal") == 0 || strcmp(c->backend->name, "opengl") == 0 ||
                strcmp(c->backend->name, "d3d11") == 0)
                   ? 1
                   : 0;
    }
    native_texture_flag = canvas3d_native_texture_capability_from_name(name);
    if (native_texture_flag)
        return (rt_canvas3d_get_backend_capabilities(obj) & native_texture_flag) ? 1 : 0;
    texture_capability = canvas3d_texture_capability_from_name(name);
    if (texture_capability >= 0) {
        rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
        if (!c || !c->backend)
            return 0;
        return texture_capability ? 1 : 0;
    }
    flag = canvas3d_capability_from_name(name);
    if (!flag)
        return 0;
    return (rt_canvas3d_get_backend_capabilities(obj) & flag) ? 1 : 0;
}

/// @brief Force all skinned draws through the CPU path (bisection/debug override).
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Non-zero to bypass GPU skinning.
void rt_canvas3d_set_force_cpu_skinning(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (c)
        c->force_cpu_skinning = enabled ? 1 : 0;
}

/// @brief `Canvas3D.ForceCpuSkinning` — read the retained CPU-skinning override (ADR 0233).
/// @param obj Borrowed Canvas3D handle.
/// @return Nonzero while skinned draws are forced through the CPU path.
int8_t rt_canvas3d_get_force_cpu_skinning(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->force_cpu_skinning ? 1 : 0;
}

/// @brief Lifetime count of skinned draws routed to GPU vertex-shader skinning.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime draw count.
int64_t rt_canvas3d_get_gpu_skinned_draw_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->gpu_skinned_draw_count > 0 ? c->gpu_skinned_draw_count : 0;
}

/// @brief Lifetime bone-palette bytes handed to the backend for GPU skinning.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime byte count.
int64_t rt_canvas3d_get_skinning_upload_bytes(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->skinning_upload_bytes > 0 ? c->skinning_upload_bytes : 0;
}

/// @brief Per-pass draw submissions for the latest frame (plan 30).
/// @details Pass ids follow Game3D.RenderPass; PostFX/Present report 0 in
///   v1 (no per-draw work is attributed to them yet).
/// @param obj Borrowed Canvas3D handle.
/// @param pass RenderPass id in the inclusive range zero through five.
/// @return Latest attributed draw count, or zero for invalid input.
int64_t rt_canvas3d_pass_draw_count(void *obj, int64_t pass) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || pass < 0 || pass > 5)
        return 0;
    return c->last_pass_draw_count[pass];
}

/// @brief Per-pass instances (instanced draws expanded) for the latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @param pass RenderPass id in the inclusive range zero through five.
/// @return Latest attributed instance count, or zero for invalid input.
int64_t rt_canvas3d_pass_instance_count(void *obj, int64_t pass) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || pass < 0 || pass > 5)
        return 0;
    return c->last_pass_instance_count[pass];
}

/// @brief Number of main 3D draw submissions queued by the latest ended frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest main draw count, or zero for invalid input.
int64_t rt_canvas3d_get_draw_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_draw_count : 0;
}

/// @brief Number of latest Scene3D draw submissions skipped by visibility culling.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest aggregate visibility-cull count, or zero.
int64_t rt_canvas3d_get_occluded_draw_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_occluded_draw_count : 0;
}

/// @brief Set the shadow-light slot budget (clamped 1..VGFX3D_MAX_SHADOW_LIGHTS).
/// @param obj Borrowed Canvas3D handle.
/// @param budget Requested slot count, clamped to the supported interval.
void rt_canvas3d_set_shadow_budget(void *obj, int64_t budget) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (budget < 1)
        budget = 1;
    if (budget > VGFX3D_MAX_SHADOW_LIGHTS)
        budget = VGFX3D_MAX_SHADOW_LIGHTS;
    c->shadow_budget = (int32_t)budget;
}

/// @brief `Canvas3D.ShadowBudget` — read the retained shadow-light slot budget (ADR 0233).
/// @param obj Borrowed Canvas3D handle.
/// @return Retained slot budget, or zero for an invalid handle.
int64_t rt_canvas3d_get_shadow_budget(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? (int64_t)c->shadow_budget : 0;
}

/// @brief Shadow slots rendered in the latest frame (cascades included).
/// @param obj Borrowed Canvas3D handle.
/// @return Latest used slot count, or zero.
int64_t rt_canvas3d_get_shadow_slots_used(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_shadow_slots_used : 0;
}

/// @brief Shadow-requesting lights denied a slot in the latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest denied request count, or zero.
int64_t rt_canvas3d_get_shadow_requests_dropped(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_shadow_requests_dropped : 0;
}

/// @brief Set the per-cluster light-index capacity (clamped 8..64; default 64).
/// @param obj Borrowed Canvas3D handle.
/// @param budget Requested indices per cluster, clamped to eight through 64.
void rt_canvas3d_set_cluster_light_budget(void *obj, int64_t budget) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (budget < 8)
        budget = 8;
    if (budget > 64)
        budget = 64;
    c->cluster_light_budget = (int32_t)budget;
}

/// @brief `Canvas3D.ClusterLightBudget` — read the retained per-cluster capacity (ADR 0233).
/// @param obj Borrowed Canvas3D handle.
/// @return Retained per-cluster light-index capacity, or zero for an invalid handle.
int64_t rt_canvas3d_get_cluster_light_budget(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? (int64_t)c->cluster_light_budget : 0;
}

/// @brief Lifetime count of cluster light-index entries truncated by capacity.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime truncation count.
int64_t rt_canvas3d_get_cluster_overflow_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->cluster_overflow_total : 0;
}

/// @brief Enabled lights truncated by the forward-path light limit this frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest dropped-light count, or zero.
int64_t rt_canvas3d_get_dropped_light_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_dropped_light_count : 0;
}

/// @brief Instances routed through the per-draw instanced fallback (blend/rebase)
///        in the current/latest frame. Opaque batches use the backend hook and
///        contribute zero; sustained non-zero values flag material setups that
///        forgo real instancing.
/// @param obj Borrowed Canvas3D handle.
/// @return Current or latest fallback instance count.
int64_t rt_canvas3d_get_instanced_fallback_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_instanced_fallback_count : 0;
}

/// @brief Instances skipped because a chunked fallback queue reservation actually failed.
/// @param obj Borrowed Canvas3D handle.
/// @return Current or latest dropped fallback instance count.
int64_t rt_canvas3d_get_instanced_fallback_dropped_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_instanced_fallback_dropped_count : 0;
}

/// @brief Return the sticky status of the most recently recorded Canvas3D submission failure.
/// @details Successful draws do not clear this diagnostic; callers explicitly reset it after
///   observing the condition. This preserves evidence across legacy `void` draws and degraded
///   snapshot paths.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return A `RT_CANVAS3D_SUBMISSION_*` code, or zero for an invalid handle/no failure.
int64_t rt_canvas3d_get_last_submission_status(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->last_submission_status > 0 ? (int64_t)c->last_submission_status
                                              : RT_CANVAS3D_SUBMISSION_OK;
}

/// @brief Return the saturating Canvas3D submission-failure count since construction/reset.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Non-negative failure count, or zero for an invalid handle.
int64_t rt_canvas3d_get_submission_failure_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->submission_failure_count > 0 ? c->submission_failure_count : 0;
}

/// @brief Clear the sticky Canvas3D submission status and its cumulative diagnostic count.
/// @details Queue contents, snapshot storage, and active frame state are deliberately untouched.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_reset_submission_diagnostics(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->last_submission_status = (int32_t)RT_CANVAS3D_SUBMISSION_OK;
    c->submission_failure_count = 0;
}

/// @brief Lifetime count of window/input events dropped from the public PollEvent ring.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime dropped-event count.
int64_t rt_canvas3d_get_event_drop_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->event_type_dropped_count : 0;
}

/// @brief Mesh snapshot bytes copied by the current frame, or latest ended frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative byte count, saturated at INT64_MAX.
int64_t rt_canvas3d_get_mesh_snapshot_bytes(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return 0;
    if (c->in_frame) {
        if (c->mesh_snapshot_bytes > (size_t)INT64_MAX)
            return INT64_MAX;
        return (int64_t)c->mesh_snapshot_bytes;
    }
    return c->last_mesh_snapshot_bytes;
}

/// @brief Mesh snapshot allocation/budget denials in the current/latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Current or latest snapshot denial count.
int64_t rt_canvas3d_get_mesh_snapshot_drop_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_mesh_snapshot_drop_count : 0;
}

/// @brief Requested mesh snapshot bytes denied in the current/latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Current or latest denied byte count.
int64_t rt_canvas3d_get_mesh_snapshot_dropped_bytes(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_mesh_snapshot_dropped_bytes : 0;
}

/// @brief Per-frame mesh snapshot byte budget used by deferred geometry snapshots.
/// @param obj Borrowed Canvas3D handle accepted for registry consistency.
/// @return Compile-time per-frame byte budget.
int64_t rt_canvas3d_get_mesh_snapshot_budget_bytes(void *obj) {
    (void)obj;
    return (int64_t)RT_CANVAS3D_MESH_SNAPSHOT_FRAME_BYTE_BUDGET;
}

/// @brief Number of latest draw submissions rejected by CPU frustum culling.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest frustum-rejection count.
int64_t rt_canvas3d_get_frustum_culled_draw_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_frustum_culled_draw_count : 0;
}

/// @brief Number of latest draw submissions rejected by the CPU occlusion grid.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest CPU occlusion rejection count.
int64_t rt_canvas3d_get_cpu_occluded_draw_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_cpu_occluded_draw_count : 0;
}

/// @brief Number of opaque draws tested by the CPU occlusion grid in the latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest occlusion-candidate count.
int64_t rt_canvas3d_get_occlusion_candidate_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_occlusion_candidate_count : 0;
}

/// @brief Texture payload bytes uploaded to backend storage in the latest ended frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest backend texture-upload byte count.
int64_t rt_canvas3d_get_texture_upload_bytes(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_texture_upload_bytes : 0;
}

/// @brief Latest completed backend GPU frame time in microseconds.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest non-negative GPU time, or zero when unavailable.
int64_t rt_canvas3d_get_frame_gpu_time_us(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->last_frame_gpu_time_us : 0;
}

/// @brief `Canvas3D.PassCpuMs(pass)` — CPU milliseconds one render stage took
///   during the last flushed frame.
/// @details Pass ids: 0 = shadow pass, 1 = main pass (opaque + transparent +
///   post-FX submission), 2 = screen overlay pass, 3 = backend end-of-frame
///   (encode/present). Diagnostics only — a profiler HUD can render these
///   without touching simulation state. Unknown ids return 0.
/// @param obj Borrowed Canvas3D handle.
/// @param pass Pass index in the supported diagnostic range.
/// @return Latest CPU duration in milliseconds, or zero for invalid input.
double rt_canvas3d_get_pass_cpu_ms(void *obj, int64_t pass) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || pass < 0 || pass >= RT_CANVAS3D_PASS_COUNT)
        return 0.0;
    return c->pass_cpu_ms[pass];
}

/// @brief `Canvas3D.get_PassCount` — number of PassCpuMs stages (currently 4).
/// @param obj Borrowed Canvas3D handle accepted for registry consistency.
/// @return Number of available pass timing slots.
int64_t rt_canvas3d_get_pass_count(void *obj) {
    (void)obj;
    return RT_CANVAS3D_PASS_COUNT;
}

/// @brief Backend draw submissions issued since the latest public frame begin.
/// @param obj Borrowed Canvas3D handle.
/// @return Current frame backend submission count.
int64_t rt_canvas3d_get_draws_submitted(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->frame_draws_submitted : 0;
}

/// @brief World-AABB transform computations performed since the latest public frame begin.
/// @param obj Borrowed Canvas3D handle.
/// @return Current frame AABB transform count.
int64_t rt_canvas3d_get_aabb_transforms(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->frame_aabb_transforms : 0;
}

/// @brief Stable deferred sort passes run since the latest public frame begin.
/// @param obj Borrowed Canvas3D handle.
/// @return Current frame sort-pass count.
int64_t rt_canvas3d_get_sort_passes(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->frame_sort_passes : 0;
}

/// @brief Material/backend state-group transitions observed during backend submission.
/// @param obj Borrowed Canvas3D handle.
/// @return Current frame backend state-transition count.
int64_t rt_canvas3d_get_backend_state_changes(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->frame_backend_state_changes : 0;
}

/// @brief Set the active backend's per-frame texture upload budget.
/// @param obj Borrowed Canvas3D handle.
/// @param bytes Non-negative byte budget, or negative for unlimited uploads.
void rt_canvas3d_set_texture_upload_budget(void *obj, int64_t bytes) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    uint64_t budget = bytes < 0 ? UINT64_MAX : (uint64_t)bytes;
    if (c && c->backend && c->backend->set_texture_upload_budget)
        c->backend->set_texture_upload_budget(c->backend_ctx, budget);
}

/// @brief Texture payload bytes still waiting for backend texture upload budget.
/// @param obj Borrowed Canvas3D handle.
/// @return Pending bytes saturated at INT64_MAX, or zero when unsupported.
int64_t rt_canvas3d_get_texture_upload_pending_bytes(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    uint64_t bytes = 0;
    if (c && c->backend && c->backend->get_texture_upload_pending_bytes)
        bytes = c->backend->get_texture_upload_pending_bytes(c->backend_ctx);
    return bytes > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)bytes;
}

/// @brief Clamp a backend unsigned telemetry counter into the public signed runtime range.
/// @param value Backend-owned monotonically increasing counter.
/// @return @p value as signed 64-bit, saturated at INT64_MAX.
static int64_t canvas3d_backend_counter_to_i64(uint64_t value) {
    return value > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)value;
}

/// @brief Copy the active backend's optional diagnostics snapshot.
/// @details Backends expose this through a late vtable hook so Canvas3D does not need to know
///          concrete backend context layouts. Missing hooks return an all-zero snapshot.
/// @param c Canvas3D payload, may be NULL.
/// @return Backend telemetry snapshot, zero-filled when unsupported.
static vgfx3d_backend_stats_t canvas3d_get_backend_stats_snapshot(rt_canvas3d *c) {
    vgfx3d_backend_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (c && c->backend && c->backend->get_backend_stats)
        c->backend->get_backend_stats(c->backend_ctx, &stats);
    return stats;
}

/// @brief Successful draw calls emitted by the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Saturated backend draw-call counter, or zero when unsupported.
int64_t rt_canvas3d_get_backend_draw_calls(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    vgfx3d_backend_stats_t stats = canvas3d_get_backend_stats_snapshot(c);
    return canvas3d_backend_counter_to_i64(stats.draw_calls);
}

/// @brief Draw commands rejected inside the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Saturated backend dropped-draw counter, or zero when unsupported.
int64_t rt_canvas3d_get_backend_dropped_draws(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    vgfx3d_backend_stats_t stats = canvas3d_get_backend_stats_snapshot(c);
    return canvas3d_backend_counter_to_i64(stats.dropped_draws);
}

/// @brief Static mesh cache hits observed by the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Saturated cache-hit counter, or zero when unsupported.
int64_t rt_canvas3d_get_backend_mesh_cache_hits(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    vgfx3d_backend_stats_t stats = canvas3d_get_backend_stats_snapshot(c);
    return canvas3d_backend_counter_to_i64(stats.mesh_cache_hits);
}

/// @brief Static mesh cache misses observed by the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Saturated cache-miss counter, or zero when unsupported.
int64_t rt_canvas3d_get_backend_mesh_cache_misses(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    vgfx3d_backend_stats_t stats = canvas3d_get_backend_stats_snapshot(c);
    return canvas3d_backend_counter_to_i64(stats.mesh_cache_misses);
}

/// @brief Transient mesh uploads performed by the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Saturated stream-upload counter, or zero when unsupported.
int64_t rt_canvas3d_get_backend_mesh_stream_uploads(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    vgfx3d_backend_stats_t stats = canvas3d_get_backend_stats_snapshot(c);
    return canvas3d_backend_counter_to_i64(stats.mesh_stream_uploads);
}

/// @brief Fallback texture binds observed by the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Saturated fallback-bind counter, or zero when unsupported.
int64_t rt_canvas3d_get_backend_texture_fallback_binds(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    vgfx3d_backend_stats_t stats = canvas3d_get_backend_stats_snapshot(c);
    return canvas3d_backend_counter_to_i64(stats.texture_fallback_binds);
}

/// @brief Active backend present path: 0 unknown, 1 direct GPU drawable, 2 offscreen resolve.
/// @param obj Borrowed Canvas3D handle.
/// @return Backend present-path enum value, or zero when unsupported.
int64_t rt_canvas3d_get_backend_present_path(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    vgfx3d_backend_stats_t stats = canvas3d_get_backend_stats_snapshot(c);
    return stats.present_path;
}

/// @brief Grow the reusable GPU-readback staging buffer without losing the previous allocation.
/// @param c Canvas3D owning reusable byte storage.
/// @param required Positive minimum capacity in bytes.
/// @return Non-zero when at least @p required bytes are available.
static int canvas3d_reserve_readback_scratch(rt_canvas3d *c, size_t required) {
    size_t capacity;
    uint8_t *next;
    if (!c || required == 0 || required > RT_CANVAS3D_READBACK_SCRATCH_MAX_BYTES)
        return 0;

    if (rt_canvas3d_readback_storage_is_valid(c)) {
        c->readback_rgba_scratch = c->readback_rgba_owned;
        c->readback_rgba_scratch_capacity = c->readback_rgba_storage_capacity;
    } else {
        /* A damaged ownership tuple cannot authorize realloc/free of either pointer. */
        c->readback_rgba_scratch = NULL;
        c->readback_rgba_scratch_capacity = 0u;
        c->readback_rgba_owned = NULL;
        c->readback_rgba_storage_capacity = 0u;
        c->readback_rgba_storage_cookie = 0u;
    }
    if (required <= c->readback_rgba_storage_capacity && c->readback_rgba_owned)
        return 1;
    capacity = c->readback_rgba_storage_capacity > 0 ? c->readback_rgba_storage_capacity : 4096u;
    while (capacity < required) {
        size_t grown = capacity + capacity / 2u;
        if (grown <= capacity || grown > RT_CANVAS3D_READBACK_SCRATCH_MAX_BYTES) {
            capacity = required;
            break;
        }
        capacity = grown;
    }
    next = (uint8_t *)realloc(c->readback_rgba_owned, capacity);
    if (!next)
        return 0;
    c->readback_rgba_owned = next;
    c->readback_rgba_storage_capacity = capacity;
    c->readback_rgba_storage_cookie = rt_canvas3d_readback_storage_cookie(next, capacity);
    c->readback_rgba_scratch = next;
    c->readback_rgba_scratch_capacity = capacity;
    return 1;
}

/// @brief Capture the current canvas contents into an existing same-size Pixels object.
/// @details Three-way capture path, picked by what's bound:
///          1. RTT bound → call `rendertarget_sync_color_if_needed` to
///             pull GPU contents back to CPU, then RGBA-pack each row
///             into the Pixels buffer.
///          2. GPU backend with `readback_rgba` → reuse the canvas-owned
///             RGBA scratch buffer, ask the backend to fill it, repack.
///          3. Software backend / fallback → copy directly from the
///             window's CPU framebuffer.
///          The 0xRRGGBBAA pack here matches the `rt_pixels` storage
///          convention (top byte = red), so the screenshot can be saved
///          to BMP/PNG via `Pixels.Save` without a swizzle pass.
/// @param c Borrowed Canvas3D whose active output is captured.
/// @param pv Mutable same-size Pixels destination.
/// @return 1 after a successful copy, or 0 on invalid layout/readback/allocation failure.
static int canvas3d_screenshot_into(rt_canvas3d *c, rt_pixels_impl *pv) {
    int32_t shot_w;
    int32_t shot_h;
    int32_t source_w;
    int32_t source_h;
    int copied = 0;
    if (!c || !pv || !pv->data)
        return 0;
    shot_w = c->render_target ? c->render_target->width : c->width;
    shot_h = c->render_target ? c->render_target->height : c->height;
    source_w = shot_w;
    source_h = shot_h;
    if (shot_w <= 0 || shot_h <= 0 || shot_w > VGFX3D_RENDERTARGET_DIM_MAX ||
        shot_h > VGFX3D_RENDERTARGET_DIM_MAX || pv->width != shot_w || pv->height != shot_h)
        return 0;

    if (c->render_target && vgfx3d_rendertarget_ensure_color(c->render_target)) {
        if (!vgfx3d_rendertarget_sync_color_if_needed(c->render_target))
            return 0;
        copied = canvas3d_pack_rgba_to_pixels(pv,
                                              c->render_target->color_buf,
                                              shot_w,
                                              shot_h,
                                              c->render_target->stride,
                                              shot_w,
                                              shot_h);
        if (copied)
            pixels_touch(pv);
        return copied;
    }

    if (c->framebuffer_width > 0 && c->framebuffer_height > 0) {
        source_w = c->framebuffer_width;
        source_h = c->framebuffer_height;
    }

    if (source_w <= 0 || source_h <= 0 || source_w > VGFX3D_RENDERTARGET_DIM_MAX ||
        source_h > VGFX3D_RENDERTARGET_DIM_MAX)
        return 0;

    if (c->backend && c->backend != &vgfx3d_software_backend && c->backend->readback_rgba) {
        const size_t row_bytes = (size_t)source_w * 4u;
        size_t required;
        if ((size_t)source_w > SIZE_MAX / 4u || row_bytes > INT32_MAX || row_bytes == 0u ||
            (size_t)source_h > SIZE_MAX / row_bytes)
            return 0;
        required = (size_t)source_h * row_bytes;
        if (canvas3d_reserve_readback_scratch(c, required) &&
            c->backend->readback_rgba(
                c->backend_ctx, c->readback_rgba_scratch, source_w, source_h, (int32_t)row_bytes)) {
            copied = canvas3d_pack_rgba_to_pixels(pv,
                                                  c->readback_rgba_scratch,
                                                  source_w,
                                                  source_h,
                                                  (int32_t)row_bytes,
                                                  shot_w,
                                                  shot_h);
            if (copied) {
                pixels_touch(pv);
                return 1;
            }
        }
    }

    if (c->gfx_win) {
        vgfx_framebuffer_t fb;
        if (!vgfx_get_framebuffer(c->gfx_win, &fb) || !fb.pixels || fb.width <= 0 ||
            fb.height <= 0 || (int64_t)fb.stride < (int64_t)fb.width * 4)
            return 0;
        copied = canvas3d_pack_rgba_to_pixels(
            pv, fb.pixels, fb.width, fb.height, fb.stride, shot_w, shot_h);
        if (copied)
            pixels_touch(pv);
        return copied;
    }
    return 0;
}

/// @brief Capture the current canvas contents into a freshly allocated Pixels object.
/// @param obj Borrowed Canvas3D handle.
/// @return A new Pixels object, or NULL on invalid size, allocation, or readback failure.
void *rt_canvas3d_screenshot(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    const int32_t shot_w = c && c->render_target ? c->render_target->width : c ? c->width : 0;
    const int32_t shot_h = c && c->render_target ? c->render_target->height : c ? c->height : 0;
    void *pixels;
    if (!c || shot_w <= 0 || shot_h <= 0)
        return NULL;
    pixels = rt_pixels_new((int64_t)shot_w, (int64_t)shot_h);
    if (!pixels)
        return NULL;
    if (!canvas3d_screenshot_into(c, rt_pixels_checked_impl_or_null(pixels))) {
        if (rt_obj_release_check0(pixels))
            rt_obj_free(pixels);
        return NULL;
    }
    return pixels;
}

/// @brief Copy the current canvas contents into an existing same-size Pixels object.
/// @details Reuses canvas-owned GPU staging storage after the first large-enough readback, avoiding
///          per-frame allocation in capture loops. Render-target and software paths allocate no
///          staging storage. The destination generation advances on success.
/// @param obj Borrowed Canvas3D handle.
/// @param pixels Borrowed mutable Pixels handle whose dimensions must match the output.
/// @return 1 on success; 0 for invalid handles, size mismatch, or readback failure.
int8_t rt_canvas3d_try_copy_screenshot_to(void *obj, void *pixels) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    rt_pixels_impl *pv = rt_pixels_checked_impl_or_null(pixels);
    return canvas3d_screenshot_into(c, pv) ? 1 : 0;
}

/// @brief Draw an axis-aligned bounding box as 12 wireframe edges from raw min/max corner arrays.
/// @param obj Borrowed Canvas3D handle.
/// @param min_v Borrowed three-double minimum corner.
/// @param max_v Borrowed three-double maximum corner.
/// @param color Packed RGB runtime color.
void rt_canvas3d_draw_aabb_wire_raw(void *obj,
                                    const double *min_v,
                                    const double *max_v,
                                    int64_t color) {
    if (!obj || !min_v || !max_v)
        return;
    for (int i = 0; i < 3; i++)
        if (!isfinite(min_v[i]) || !isfinite(max_v[i]))
            return;
    double mn[3] = {min_v[0], min_v[1], min_v[2]};
    double mx[3] = {max_v[0], max_v[1], max_v[2]};
    double corners[8][3];
    for (int i = 0; i < 8; i++) {
        corners[i][0] = (i & 1) ? mx[0] : mn[0];
        corners[i][1] = (i & 2) ? mx[1] : mn[1];
        corners[i][2] = (i & 4) ? mx[2] : mn[2];
    }

    static const int edges[12][2] = {{0, 1},
                                     {1, 3},
                                     {3, 2},
                                     {2, 0},
                                     {4, 5},
                                     {5, 7},
                                     {7, 6},
                                     {6, 4},
                                     {0, 4},
                                     {1, 5},
                                     {2, 6},
                                     {3, 7}};
    for (int e = 0; e < 12; e++)
        rt_canvas3d_draw_line3d_raw(obj, corners[edges[e][0]], corners[edges[e][1]], color);
}

/// @brief Draw an axis-aligned bounding box (12 lines) between `min_v` and `max_v` Vec3s.
/// Useful for collision/culling debug visualization.
/// @param obj Borrowed Canvas3D handle.
/// @param min_v Borrowed Vec3 minimum corner.
/// @param max_v Borrowed Vec3 maximum corner.
/// @param color Packed RGB runtime color.
void rt_canvas3d_draw_aabb_wire(void *obj, void *min_v, void *max_v, int64_t color) {
    double mn[3];
    double mx[3];
    if (!min_v || !max_v)
        return;
    mn[0] = rt_vec3_x(min_v);
    mn[1] = rt_vec3_y(min_v);
    mn[2] = rt_vec3_z(min_v);
    mx[0] = rt_vec3_x(max_v);
    mx[1] = rt_vec3_y(max_v);
    mx[2] = rt_vec3_z(max_v);
    rt_canvas3d_draw_aabb_wire_raw(obj, mn, mx, color);
}

/// @brief Draw three orthogonal great circles approximating a sphere (XY, XZ, YZ planes) at
/// `center` with `radius`. Cheaper than tessellating a real sphere for debug viz.
/// @param obj Borrowed Canvas3D handle.
/// @param center Borrowed Vec3 sphere center.
/// @param radius Positive finite radius in world units.
/// @param color Packed RGB runtime color.
void rt_canvas3d_draw_sphere_wire(void *obj, void *center, double radius, int64_t color) {
    if (!obj || !center || !isfinite(radius) || radius <= 0.0)
        return;
    const double cx = rt_vec3_x(center);
    const double cy = rt_vec3_y(center);
    const double cz = rt_vec3_z(center);
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(cz) || radius > DBL_MAX - fabs(cx) ||
        radius > DBL_MAX - fabs(cy) || radius > DBL_MAX - fabs(cz))
        return;
    const int segs = 24;
    const double step = 2.0 * 3.14159265358979323846 / segs;

    for (int i = 0; i < segs; i++) {
        const double a0 = i * step;
        const double a1 = (i + 1) * step;
        const double c0 = cos(a0);
        const double s0 = sin(a0);
        const double c1 = cos(a1);
        const double s1 = sin(a1);

        void *a = rt_vec3_new(cx + c0 * radius, cy + s0 * radius, cz);
        void *b = rt_vec3_new(cx + c1 * radius, cy + s1 * radius, cz);
        rt_canvas3d_draw_line3d(obj, a, b, color);
        canvas3d_release_local(a);
        canvas3d_release_local(b);

        a = rt_vec3_new(cx + c0 * radius, cy, cz + s0 * radius);
        b = rt_vec3_new(cx + c1 * radius, cy, cz + s1 * radius);
        rt_canvas3d_draw_line3d(obj, a, b, color);
        canvas3d_release_local(a);
        canvas3d_release_local(b);

        a = rt_vec3_new(cx, cy + c0 * radius, cz + s0 * radius);
        b = rt_vec3_new(cx, cy + c1 * radius, cz + s1 * radius);
        rt_canvas3d_draw_line3d(obj, a, b, color);
        canvas3d_release_local(a);
        canvas3d_release_local(b);
    }
}

/// @brief Draw a ray from `origin` along `dir` for a scalar `length`.
/// @details The direction is multiplied directly rather than normalized; callers
///   should provide a unit Vec3 when @p length must equal world-space distance.
/// @param obj Borrowed Canvas3D handle.
/// @param origin Borrowed Vec3 ray origin.
/// @param dir Borrowed direction Vec3.
/// @param length Finite direction multiplier.
/// @param color Packed RGB runtime color.
void rt_canvas3d_draw_debug_ray(void *obj, void *origin, void *dir, double length, int64_t color) {
    double end_position[3];
    if (!obj || !origin || !dir || !isfinite(length))
        return;
    end_position[0] = rt_vec3_x(origin) + rt_vec3_x(dir) * length;
    end_position[1] = rt_vec3_y(origin) + rt_vec3_y(dir) * length;
    end_position[2] = rt_vec3_z(origin) + rt_vec3_z(dir) * length;
    if (!isfinite(end_position[0]) || !isfinite(end_position[1]) || !isfinite(end_position[2]))
        return;
    void *end = rt_vec3_new(end_position[0], end_position[1], end_position[2]);
    rt_canvas3d_draw_line3d(obj, origin, end, color);
    canvas3d_release_local(end);
}

/// @brief Draw an XYZ axis gizmo at `origin` with arms of length `scale`. Standard color
/// convention: red=X, green=Y, blue=Z. Useful for visualizing world / object orientation.
/// @param obj Borrowed Canvas3D handle.
/// @param origin Borrowed Vec3 gizmo origin.
/// @param scale Finite signed arm length in world units.
void rt_canvas3d_draw_axis(void *obj, void *origin, double scale) {
    double ox;
    double oy;
    double oz;
    if (!obj || !origin || !isfinite(scale))
        return;
    ox = rt_vec3_x(origin);
    oy = rt_vec3_y(origin);
    oz = rt_vec3_z(origin);
    if (!isfinite(ox) || !isfinite(oy) || !isfinite(oz) || !isfinite(ox + scale) ||
        !isfinite(oy + scale) || !isfinite(oz + scale))
        return;
    void *end = rt_vec3_new(ox + scale, oy, oz);
    rt_canvas3d_draw_line3d(obj, origin, end, 0xFF0000);
    canvas3d_release_local(end);
    end = rt_vec3_new(ox, oy + scale, oz);
    rt_canvas3d_draw_line3d(obj, origin, end, 0x00FF00);
    canvas3d_release_local(end);
    end = rt_vec3_new(ox, oy, oz + scale);
    rt_canvas3d_draw_line3d(obj, origin, end, 0x0000FF);
    canvas3d_release_local(end);
}

#else
typedef int rt_canvas3d_overlay_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
