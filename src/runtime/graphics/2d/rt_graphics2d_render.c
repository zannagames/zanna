//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_graphics2d_render.c
/// @brief Implements stateful shape, text, nine-slice, and debug renderers for
///        the 2D runtime.
///
/// @details The renderers in this translation unit build on the shared
/// Pixels, Canvas, font, object-lifetime, color-normalization, and blitting
/// helpers declared by `rt_graphics2d_internal.h`. Drawing APIs borrow their
/// destination handles. Objects that store a font or source image retain that
/// handle until replacement or finalization, while dynamic debug-command
/// storage is owned directly by its DebugDraw2D object.
///
// File: src/runtime/graphics/2d/rt_graphics2d_render.c
// Purpose: Higher-level 2D renderer classes — ShapeRenderer2D, TextRenderer2D,
//          SdfFont, NineSlice2D, and DebugDraw2D. Split out of rt_graphics2d.c;
//          shares the 2D foundation helpers (blit/draw/size math) via
//          rt_graphics2d_internal.h.
//
// Key invariants:
//   - Class handles are validated via the *_checked casts before use.
//   - All blitting routes through the shared rt2d_blit_pixels so tint/alpha/blend
//     behavior stays consistent with the rest of the 2D layer.
//
// Ownership/Lifetime:
//   - Class impls are GC objects. Retained font and source-image handles are
//     released on finalize, and owned command buffers are freed.
//   - Rendering helpers borrow caller-provided destination and path handles.
//
// Links: src/runtime/graphics/2d/rt_graphics2d.c (other 2D classes + helpers),
//        src/runtime/graphics/2d/rt_graphics2d_internal.h (shared helpers)
//
//===----------------------------------------------------------------------===//

#include "rt_graphics2d.h"
#include "rt_graphics2d_internal.h"

#include "rt_bitmapfont.h"
#include "rt_graphics.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Clamp a value to [min, max] (file-local copy; the name is shared by
///        several runtime modules as a static helper).
/// @param value Signed value to constrain.
/// @param min Inclusive lower bound.
/// @param max Inclusive upper bound; callers provide @p min no greater than
///            this value.
/// @return @p value constrained to the inclusive interval [@p min, @p max].
static int64_t clamp_i64(int64_t value, int64_t min, int64_t max) {
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

/// @name Renderer runtime class identifiers
/// @{
#define RT2D_SHAPERENDERER_CLASS_ID INT64_C(-0x62010B)
#define RT2D_TEXTRENDERER_CLASS_ID INT64_C(-0x62010C)
#define RT2D_SDFFONT_CLASS_ID INT64_C(-0x62010D)
#define RT2D_NINESLICE_CLASS_ID INT64_C(-0x62010E)
#define RT2D_DEBUGDRAW_CLASS_ID INT64_C(-0x62010F)
/// @}

/// @brief Mutable stroke and fill state owned by a ShapeRenderer2D object.
typedef struct {
    /// Source color for outlines, or a negative value to disable them.
    int64_t stroke;
    /// Source color for interiors, or a negative value to disable them.
    int64_t fill;
} rt_shaperenderer2d_impl;

/// @brief Retained font and draw settings owned by a TextRenderer2D object.
typedef struct {
    /// Retained BitmapFont or SpriteFont handle, or `NULL` for the Canvas font.
    void *font;
    /// Integer glyph scale in the inclusive range `[1, 64]`.
    int64_t scale;
    /// Text color encoded as `0x00RRGGBB`.
    int64_t color;
} rt_textrenderer2d_impl;

/// @brief Bitmap-backed state stored by the current SdfFont implementation.
typedef struct {
    /// Retained BitmapFont or SpriteFont handle, possibly `NULL`.
    void *bitmap_font;
    /// Stored signed-distance spread setting in `[1, 64]`.
    int64_t spread;
} rt_sdffont_impl;

/// @brief Retained source image and requested border widths for a nine-slice.
typedef struct {
    /// Retained Pixels handle containing the source image.
    void *pixels;
    /// Requested left source border width.
    int64_t left;
    /// Requested top source border height.
    int64_t top;
    /// Requested right source border width.
    int64_t right;
    /// Requested bottom source border height.
    int64_t bottom;
} rt_nineslice2d_impl;

/// @brief Compact queued primitive consumed by DebugDraw2D rendering.
/// @details Fields are interpreted according to @c type: lines use both point
///          pairs, rectangles store width and height in @c x1 and @c y1, and
///          circles store their radius in @c value.
typedef struct {
    /// Primitive discriminator: `1` line, `2` rectangle, or `3` circle.
    int32_t type;
    /// First X coordinate, or rectangle/circle center X.
    int64_t x0;
    /// First Y coordinate, or rectangle/circle center Y.
    int64_t y0;
    /// Second line X coordinate or rectangle width.
    int64_t x1;
    /// Second line Y coordinate or rectangle height.
    int64_t y1;
    /// Shape-specific auxiliary value, currently the circle radius.
    int64_t value;
    /// Packed or tagged source color normalized when the command is drawn.
    int64_t color;
} rt_debugdraw2d_cmd;

/// @brief Growable retained-command queue owned by a DebugDraw2D object.
typedef struct {
    /// Heap-allocated command storage.
    rt_debugdraw2d_cmd *cmds;
    /// Number of initialized commands.
    int64_t count;
    /// Number of allocated command slots.
    int64_t capacity;
} rt_debugdraw2d_impl;

//=============================================================================
// ShapeRenderer2D
//=============================================================================
// Stateful line / rect / circle / path drawer. Holds current stroke + fill
// colors; drawing ops write directly into a Pixels buffer via the shared
// `rt_pixels_draw_*` primitives. A negative color (the default for `fill`)
// skips that half of the draw — e.g. rect with `fill = -1` is stroke-only.

/// @brief Allocate a ShapeRenderer2D with default colors (white stroke, no fill).
/// @return A new ShapeRenderer2D handle, or `NULL` if runtime object allocation
///         fails.
void *rt_shaperenderer2d_new(void) {
    rt_shaperenderer2d_impl *renderer = (rt_shaperenderer2d_impl *)rt_obj_new_i64(
        RT2D_SHAPERENDERER_CLASS_ID, (int64_t)sizeof(rt_shaperenderer2d_impl));
    if (!renderer)
        return NULL;
    renderer->stroke = 0x00FFFFFF;
    renderer->fill = -1;
    return renderer;
}

/// @brief Set the stroke (outline) color; pass any negative value to disable stroke.
/// @param renderer Opaque ShapeRenderer2D handle to update; invalid handles are
///                 ignored.
/// @param rgba Packed RGB, packed RGBA, or tagged Color value stored verbatim
///             until drawing; any negative value disables the stroke.
void rt_shaperenderer2d_set_stroke(void *renderer, int64_t rgba) {
    if (rt2d_has_class(renderer, RT2D_SHAPERENDERER_CLASS_ID))
        ((rt_shaperenderer2d_impl *)renderer)->stroke = rgba;
}

/// @brief Set the fill color; pass any negative value to disable fill (stroke-only mode).
/// @param renderer Opaque ShapeRenderer2D handle to update; invalid handles are
///                 ignored.
/// @param rgba Packed RGB, packed RGBA, or tagged Color value stored verbatim
///             until drawing; any negative value disables the fill.
void rt_shaperenderer2d_set_fill(void *renderer, int64_t rgba) {
    if (rt2d_has_class(renderer, RT2D_SHAPERENDERER_CLASS_ID))
        ((rt_shaperenderer2d_impl *)renderer)->fill = rgba;
}

/// @brief Draw a line from `(x0, y0)` to `(x1, y1)` using the current stroke color.
/// @details Uses `rt_pixels_draw_line`; no-op if the stroke is disabled or
///          either object handle is invalid. Color normalization discards
///          alpha before the RGB-only Pixels primitive is called.
/// @param renderer Opaque ShapeRenderer2D handle providing the stroke state.
/// @param pixels Borrowed destination Pixels handle modified in place.
/// @param x0 Starting X coordinate.
/// @param y0 Starting Y coordinate.
/// @param x1 Ending X coordinate.
/// @param y1 Ending Y coordinate.
void rt_shaperenderer2d_line(
    void *renderer, void *pixels, int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    if (!rt2d_has_class(renderer, RT2D_SHAPERENDERER_CLASS_ID) || !pixels)
        return;
    rt_shaperenderer2d_impl *impl = (rt_shaperenderer2d_impl *)renderer;
    if (impl->stroke < 0)
        return;
    rt_pixels_draw_line(pixels, x0, y0, x1, y1, rt2d_draw_rgb(impl->stroke));
}

/// @brief Draw a rectangle at `(x, y)` with `width × height`.
/// @details Draws a filled solid rect first (if fill >= 0), then a 1-px
///          outline frame on top (if stroke >= 0), so the stroke is always
///          visible over the fill color.
/// @param renderer Opaque ShapeRenderer2D handle providing fill and stroke
///                 state.
/// @param pixels Borrowed destination Pixels handle modified in place.
/// @param x X coordinate of the rectangle origin.
/// @param y Y coordinate of the rectangle origin.
/// @param width Requested rectangle width passed to the Pixels primitives.
/// @param height Requested rectangle height passed to the Pixels primitives.
void rt_shaperenderer2d_rect(
    void *renderer, void *pixels, int64_t x, int64_t y, int64_t width, int64_t height) {
    if (!rt2d_has_class(renderer, RT2D_SHAPERENDERER_CLASS_ID) || !pixels)
        return;
    rt_shaperenderer2d_impl *impl = (rt_shaperenderer2d_impl *)renderer;
    if (impl->fill >= 0)
        rt_pixels_draw_box(pixels, x, y, width, height, rt2d_draw_rgb(impl->fill));
    if (impl->stroke >= 0)
        rt_pixels_draw_frame(pixels, x, y, width, height, rt2d_draw_rgb(impl->stroke));
}

/// @brief Draw a circle at `(x, y)` with the given `radius`.
/// @details Same fill-then-stroke ordering as `rt_shaperenderer2d_rect`.
///          Fill uses `rt_pixels_draw_disc`; stroke uses `rt_pixels_draw_ring`.
/// @param renderer Opaque ShapeRenderer2D handle providing fill and stroke
///                 state.
/// @param pixels Borrowed destination Pixels handle modified in place.
/// @param x X coordinate of the circle center.
/// @param y Y coordinate of the circle center.
/// @param radius Requested radius passed to the Pixels primitives.
void rt_shaperenderer2d_circle(void *renderer, void *pixels, int64_t x, int64_t y, int64_t radius) {
    if (!rt2d_has_class(renderer, RT2D_SHAPERENDERER_CLASS_ID) || !pixels)
        return;
    rt_shaperenderer2d_impl *impl = (rt_shaperenderer2d_impl *)renderer;
    if (impl->fill >= 0)
        rt_pixels_draw_disc(pixels, x, y, radius, rt2d_draw_rgb(impl->fill));
    if (impl->stroke >= 0)
        rt_pixels_draw_ring(pixels, x, y, radius, rt2d_draw_rgb(impl->stroke));
}

/// @brief Render a Path2D into `pixels` using the current stroke color.
/// @details Delegates entirely to `rt_path2d_draw_to_pixels`; fill is ignored
///          for paths because Path2D provides no closed-region fill operation.
///          Invalid handles and disabled strokes are ignored.
/// @param renderer Opaque ShapeRenderer2D handle providing the stroke color.
/// @param pixels Borrowed destination Pixels handle modified in place.
/// @param path Borrowed Path2D handle containing the line segments.
void rt_shaperenderer2d_path(void *renderer, void *pixels, void *path) {
    if (!rt2d_has_class(renderer, RT2D_SHAPERENDERER_CLASS_ID) || !pixels || !path)
        return;
    rt_shaperenderer2d_impl *impl = (rt_shaperenderer2d_impl *)renderer;
    if (impl->stroke < 0)
        return;
    rt_path2d_draw_to_pixels(path, pixels, impl->stroke);
}

//=============================================================================
// TextRenderer2D
//=============================================================================
// Wraps an optional BitmapFont or SpriteFont plus a scale + color, and exposes
// measure / draw entries that go through the existing `rt_canvas_text_*`
// primitives.
// When no font is bound, measurement and drawing fall back to the Canvas
// built-in font so a TextRenderer2D is always usable from construction.

/// @brief GC finalizer — releases the retained bitmap-backed font reference.
/// @param obj Opaque finalizing object expected to be a TextRenderer2D
///            instance; invalid handles are ignored.
static void textrenderer2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_TEXTRENDERER_CLASS_ID))
        return;
    rt_textrenderer2d_impl *renderer = (rt_textrenderer2d_impl *)obj;
    rt2d_release_ref_slot(&renderer->font);
}

/// @brief Allocate a TextRenderer2D with default state (no font, 1x scale, white).
/// @return A new TextRenderer2D handle, or `NULL` if runtime object allocation
///         fails.
void *rt_textrenderer2d_new(void) {
    rt_textrenderer2d_impl *renderer = (rt_textrenderer2d_impl *)rt_obj_new_i64(
        RT2D_TEXTRENDERER_CLASS_ID, (int64_t)sizeof(rt_textrenderer2d_impl));
    if (!renderer)
        return NULL;
    renderer->scale = 1;
    renderer->color = 0x00FFFFFF;
    rt_obj_set_finalizer(renderer, textrenderer2d_finalize);
    return renderer;
}

/// @brief Bind a bitmap-backed font to this renderer, retaining a reference.
/// @details Releases any previously held font before storing the new one,
///          following the standard retain-before-release slot discipline.
///          Pass NULL to revert to the built-in Canvas font.
/// @param renderer Opaque TextRenderer2D handle to update.
/// @param font BitmapFont or SpriteFont handle to retain, or `NULL`; invalid
///             non-null font handles leave the renderer unchanged.
void rt_textrenderer2d_set_font(void *renderer, void *font) {
    if (!rt2d_has_class(renderer, RT2D_TEXTRENDERER_CLASS_ID) ||
        (font && !rt2d_is_bitmap_font_handle(font)))
        return;
    rt_textrenderer2d_impl *impl = (rt_textrenderer2d_impl *)renderer;
    rt2d_retain_ref(font);
    rt2d_release_ref_slot(&impl->font);
    impl->font = font;
}

/// @brief Set the integer pixel scale factor; clamped to [1, 64].
/// @param renderer Opaque TextRenderer2D handle to update; invalid handles are
///                 ignored.
/// @param scale Requested positive integer glyph scale.
void rt_textrenderer2d_set_scale(void *renderer, int64_t scale) {
    if (rt2d_has_class(renderer, RT2D_TEXTRENDERER_CLASS_ID))
        ((rt_textrenderer2d_impl *)renderer)->scale = clamp_i64(scale, 1, 64);
}

/// @brief Set the text color as a packed 0x00RRGGBB value (alpha bits are masked off).
/// @param renderer Opaque TextRenderer2D handle to update; invalid handles are
///                 ignored.
/// @param rgb Source value whose low 24 bits are stored.
void rt_textrenderer2d_set_color(void *renderer, int64_t rgb) {
    if (rt2d_has_class(renderer, RT2D_TEXTRENDERER_CLASS_ID))
        ((rt_textrenderer2d_impl *)renderer)->color = rgb & 0x00FFFFFF;
}

/// @brief Measure the pixel width of `text` using the bound font and scale.
/// @details Falls back to `rt_canvas_text_width` when no bitmap-backed font is bound.
///          Width is multiplied by scale using saturating arithmetic to avoid overflow.
/// @param renderer Opaque TextRenderer2D handle, or an invalid handle to use
///                 the unscaled built-in Canvas measurement.
/// @param text Runtime string to measure.
/// @return The measured width multiplied by the configured scale and saturated
///         to `int64_t`; for an invalid renderer, the built-in unscaled width.
int64_t rt_textrenderer2d_measure_width(void *renderer, rt_string text) {
    if (!rt2d_has_class(renderer, RT2D_TEXTRENDERER_CLASS_ID))
        return rt_canvas_text_width(text);
    rt_textrenderer2d_impl *impl = (rt_textrenderer2d_impl *)renderer;
    int64_t width =
        impl->font ? rt_bitmapfont_text_width(impl->font, text) : rt_canvas_text_width(text);
    return rt2d_saturating_mul_i64(width, impl->scale);
}

/// @brief Measure the pixel height of one line of text with the bound font and scale.
/// @details The `text` argument is ignored — line height is font-uniform, not
///          string-dependent. Falls back to `rt_canvas_text_height` when no font is bound.
/// @param renderer Opaque TextRenderer2D handle, or an invalid handle to use
///                 the unscaled built-in Canvas line height.
/// @param text Unused runtime string retained for API symmetry with width
///             measurement.
/// @return The selected font height multiplied by scale and saturated to
///         `int64_t`; for an invalid renderer, the built-in unscaled height.
int64_t rt_textrenderer2d_measure_height(void *renderer, rt_string text) {
    (void)text;
    if (!rt2d_has_class(renderer, RT2D_TEXTRENDERER_CLASS_ID))
        return rt_canvas_text_height();
    rt_textrenderer2d_impl *impl = (rt_textrenderer2d_impl *)renderer;
    int64_t height = impl->font ? rt_bitmapfont_text_height(impl->font) : rt_canvas_text_height();
    return rt2d_saturating_mul_i64(height, impl->scale);
}

/// @brief Draw `text` at `(x, y)` into `canvas` using the bound font, scale, and color.
/// @details If a bitmap-backed font is bound, uses `rt_canvas_text_font_scaled`; otherwise
///          falls back to `rt_canvas_text_scaled` with the built-in Canvas font.
/// @param renderer Opaque TextRenderer2D handle providing font and draw state.
/// @param canvas Borrowed destination Canvas handle modified in place.
/// @param x X coordinate of the text origin.
/// @param y Y coordinate of the text origin.
/// @param text Runtime string to draw.
void rt_textrenderer2d_draw(void *renderer, void *canvas, int64_t x, int64_t y, rt_string text) {
    if (!rt2d_has_class(renderer, RT2D_TEXTRENDERER_CLASS_ID) || !canvas)
        return;
    rt_textrenderer2d_impl *impl = (rt_textrenderer2d_impl *)renderer;
    if (impl->font)
        rt_canvas_text_font_scaled(canvas, x, y, text, impl->font, impl->scale, impl->color);
    else
        rt_canvas_text_scaled(canvas, x, y, text, impl->scale, impl->color);
}

//=============================================================================
// SdfFont
//=============================================================================
// Forward-compatible name for a signed-distance-field font. The current
// backend wraps a BitmapFont or SpriteFont and stores a `spread` parameter; real SDF
// raster drawing is a future addition. Callers should code against the
// SdfFont surface today and gain crisper scaling when the backend upgrades.

/// @brief GC finalizer — releases the retained bitmap-backed font reference.
/// @param obj Opaque finalizing object expected to be an SdfFont instance;
///            invalid handles are ignored.
static void sdffont_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_SDFFONT_CLASS_ID))
        return;
    rt_sdffont_impl *font = (rt_sdffont_impl *)obj;
    rt2d_release_ref_slot(&font->bitmap_font);
}

/// @brief Wrap an optional bitmap-backed font as an SdfFont.
/// @details `spread` is clamped to `[1, 64]`. Consumers that support real SDF
///          rendering will use `spread` directly; the current bitmap-backed
///          implementation records it but ignores it at draw time. The
///          constructor takes its own retained reference and accepts `NULL`.
/// @param bitmap_font BitmapFont or SpriteFont handle to retain, or `NULL`;
///                    other non-null runtime objects are rejected.
/// @param spread Requested signed-distance spread setting.
/// @return A new SdfFont handle, or `NULL` for an invalid font handle or
///         allocation failure.
void *rt_sdffont_new(void *bitmap_font, int64_t spread) {
    if (bitmap_font && !rt2d_is_bitmap_font_handle(bitmap_font))
        return NULL;
    rt2d_retain_ref(bitmap_font);
    rt_sdffont_impl *font =
        (rt_sdffont_impl *)rt_obj_new_i64(RT2D_SDFFONT_CLASS_ID, (int64_t)sizeof(rt_sdffont_impl));
    if (!font) {
        void *owned_font = bitmap_font;
        rt2d_release_ref_slot(&owned_font);
        return NULL;
    }
    font->bitmap_font = bitmap_font;
    font->spread = clamp_i64(spread, 1, 64);
    rt_obj_set_finalizer(font, sdffont_finalize);
    return font;
}

/// @brief Return the underlying bitmap-backed font without retaining it.
/// @param font Opaque SdfFont handle to query.
/// @return The borrowed BitmapFont or SpriteFont handle stored by @p font, or
///         `NULL` when the SdfFont is invalid or has no backing font.
void *rt_sdffont_get_bitmap_font(void *font) {
    return rt2d_has_class(font, RT2D_SDFFONT_CLASS_ID) ? ((rt_sdffont_impl *)font)->bitmap_font
                                                       : NULL;
}

/// @brief Return the SDF spread value stored at construction time (range [1, 64]).
/// @param font Opaque SdfFont handle to query.
/// @return The stored spread in `[1, 64]`, or `0` when @p font is invalid.
int64_t rt_sdffont_get_spread(void *font) {
    return rt2d_has_class(font, RT2D_SDFFONT_CLASS_ID) ? ((rt_sdffont_impl *)font)->spread : 0;
}

//=============================================================================
// NineSlice2D
//=============================================================================
// Stretchable UI image — when space permits, four corner tiles stay fixed-size,
// four edge tiles stretch along one axis, and the center tile stretches both
// axes. Used for resizable panels, buttons, and window frames where the border
// decoration should not smear under ordinary enlargement. The
// `left / top / right / bottom` parameters are source-image border widths,
// measured inward from each edge.

/// @brief GC finalizer — releases the retained source Pixels.
/// @param obj Opaque finalizing object expected to be a NineSlice2D instance;
///            invalid handles are ignored.
static void nineslice2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_NINESLICE_CLASS_ID))
        return;
    rt_nineslice2d_impl *slice = (rt_nineslice2d_impl *)obj;
    rt2d_release_ref_slot(&slice->pixels);
}

/// @brief Wrap a source Pixels image as a nine-slice with the given border widths.
/// @details Each horizontal or vertical border is independently clamped to its
///          corresponding source dimension at construction. Drawing performs
///          a second clamp so opposing borders cannot overlap. The caller
///          retains ownership of its @p pixels reference; this object takes an
///          additional retained reference.
/// @param pixels Valid Pixels handle containing the nine-slice source image.
/// @param left Requested left border width in source pixels.
/// @param top Requested top border height in source pixels.
/// @param right Requested right border width in source pixels.
/// @param bottom Requested bottom border height in source pixels.
/// @return A new NineSlice2D handle, or `NULL` for an invalid source or
///         allocation failure.
void *rt_nineslice2d_new(void *pixels, int64_t left, int64_t top, int64_t right, int64_t bottom) {
    if (!pixels)
        return NULL;
    if (!rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl)))
        return NULL;
    int64_t pixels_width = rt_pixels_width(pixels);
    int64_t pixels_height = rt_pixels_height(pixels);
    rt2d_retain_ref(pixels);
    rt_nineslice2d_impl *slice = (rt_nineslice2d_impl *)rt_obj_new_i64(
        RT2D_NINESLICE_CLASS_ID, (int64_t)sizeof(rt_nineslice2d_impl));
    if (!slice) {
        void *owned_pixels = pixels;
        rt2d_release_ref_slot(&owned_pixels);
        return NULL;
    }
    slice->pixels = pixels;
    slice->left = clamp_i64(left, 0, pixels_width);
    slice->top = clamp_i64(top, 0, pixels_height);
    slice->right = clamp_i64(right, 0, pixels_width);
    slice->bottom = clamp_i64(bottom, 0, pixels_height);
    rt_obj_set_finalizer(slice, nineslice2d_finalize);
    return slice;
}

/// @brief Copy one rectangular region from `source` into `target`, scaling as
///        needed. Used by the nine-slice draw to place each of the 9 sub-rects.
/// @details Fast path when source and destination sizes match — a direct
///          `rt2d_blit_pixels` with no intermediate allocation. When they don't,
///          allocates a temporary region copy (via `rt2d_copy_region_pixels`),
///          scales it to the destination dimensions (`rt_pixels_scale`), and
///          blits the scaled result. Both temporaries are released before
///          returning. No-op if any dimension is non-positive.
/// @param target Borrowed destination Pixels handle modified in place.
/// @param dx X coordinate of the destination rectangle.
/// @param dy Y coordinate of the destination rectangle.
/// @param dw Destination width.
/// @param dh Destination height.
/// @param source Borrowed source Pixels handle.
/// @param sx X coordinate of the source rectangle.
/// @param sy Y coordinate of the source rectangle.
/// @param sw Source width.
/// @param sh Source height.
static void nineslice_copy_scaled(void *target,
                                  int64_t dx,
                                  int64_t dy,
                                  int64_t dw,
                                  int64_t dh,
                                  void *source,
                                  int64_t sx,
                                  int64_t sy,
                                  int64_t sw,
                                  int64_t sh) {
    if (!target || !source || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;
    if (dw == sw && dh == sh) {
        rt2d_blit_pixels(target, dx, dy, source, sx, sy, sw, sh, -1, 255, RT_GRAPHICS2D_BLEND_ALPHA);
        return;
    }
    void *region = rt2d_copy_region_pixels(source, sx, sy, sw, sh);
    if (!region)
        return;
    void *scaled = rt_pixels_scale(region, dw, dh);
    if (scaled)
        rt2d_blit_pixels(target, dx, dy, scaled, 0, 0, dw, dh, -1, 255, RT_GRAPHICS2D_BLEND_ALPHA);
    rt2d_release_ref_slot(&scaled);
    rt2d_release_ref_slot(&region);
}

/// @brief Render the nine-slice into `target` at position `(x, y)`, stretched to
///        `width × height`.
/// @details The core layout computes two rectangle sets:
///          - **Source:** `sl`, `sr`, `st`, `sb` are the border widths, clamped
///            so they can never overlap (right is clamped to `width - left`).
///            `scw` / `sch` are the remaining center dimensions.
///          - **Destination:** `dl`, `dr`, `dt`, `db` use the same border
///            widths but clamped against the destination dimensions, so a
///            nine-slice drawn smaller than its source still produces
///            sensible (shrunken) borders. `dcw` / `dch` are the stretched
///            center dimensions.
///
///          Then nine `nineslice_copy_scaled` calls place the nine sub-rects
///          in row-major order: top-left / top-center / top-right,
///          middle-left / middle-center / middle-right, bottom-left /
///          bottom-center / bottom-right. At sufficiently large destination
///          sizes, corners remain at native size, edges stretch along one
///          axis, and the center stretches along both. Smaller destinations
///          scale border pieces to their clamped destination extents so they
///          do not overlap. Derived destination coordinates use saturating
///          addition.
/// @param slice Opaque NineSlice2D handle providing source image and borders.
/// @param target Borrowed destination Pixels handle modified in place.
/// @param x X coordinate of the destination rectangle.
/// @param y Y coordinate of the destination rectangle.
/// @param width Positive destination width.
/// @param height Positive destination height.
void rt_nineslice2d_draw_to_pixels(
    void *slice, void *target, int64_t x, int64_t y, int64_t width, int64_t height) {
    if (!rt2d_has_class(slice, RT2D_NINESLICE_CLASS_ID) || !target || width <= 0 || height <= 0)
        return;
    rt_nineslice2d_impl *impl = (rt_nineslice2d_impl *)slice;
    int64_t source_width = rt_pixels_width(impl->pixels);
    int64_t source_height = rt_pixels_height(impl->pixels);
    int64_t sl = clamp_i64(impl->left, 0, source_width);
    int64_t sr = clamp_i64(impl->right, 0, source_width - sl);
    int64_t st = clamp_i64(impl->top, 0, source_height);
    int64_t sb = clamp_i64(impl->bottom, 0, source_height - st);
    int64_t dl = clamp_i64(sl, 0, width);
    int64_t dr = clamp_i64(sr, 0, width - dl);
    int64_t dt = clamp_i64(st, 0, height);
    int64_t db = clamp_i64(sb, 0, height - dt);
    int64_t scw = source_width - sl - sr;
    int64_t sch = source_height - st - sb;
    int64_t dcw = width - dl - dr;
    int64_t dch = height - dt - db;

    int64_t x_dl = rt2d_saturating_add_i64(x, dl);
    int64_t x_dl_dcw = rt2d_saturating_add_i64(x_dl, dcw);
    int64_t y_dt = rt2d_saturating_add_i64(y, dt);
    int64_t y_dt_dch = rt2d_saturating_add_i64(y_dt, dch);

    nineslice_copy_scaled(target, x, y, dl, dt, impl->pixels, 0, 0, sl, st);
    nineslice_copy_scaled(target, x_dl, y, dcw, dt, impl->pixels, sl, 0, scw, st);
    nineslice_copy_scaled(target, x_dl_dcw, y, dr, dt, impl->pixels, sl + scw, 0, sr, st);

    nineslice_copy_scaled(target, x, y_dt, dl, dch, impl->pixels, 0, st, sl, sch);
    nineslice_copy_scaled(target, x_dl, y_dt, dcw, dch, impl->pixels, sl, st, scw, sch);
    nineslice_copy_scaled(target, x_dl_dcw, y_dt, dr, dch, impl->pixels, sl + scw, st, sr, sch);

    nineslice_copy_scaled(target, x, y_dt_dch, dl, db, impl->pixels, 0, st + sch, sl, sb);
    nineslice_copy_scaled(target, x_dl, y_dt_dch, dcw, db, impl->pixels, sl, st + sch, scw, sb);
    nineslice_copy_scaled(
        target, x_dl_dcw, y_dt_dch, dr, db, impl->pixels, sl + scw, st + sch, sr, sb);
}

//=============================================================================
// DebugDraw2D
//=============================================================================
// Retained queue of debug line / rect / circle primitives. Typical usage:
// gameplay code accumulates shapes during the logic update, the renderer
// draws them all at the end of the frame. Rendering does not consume the
// queue. `Clear` resets its logical length while preserving allocated storage,
// so later queueing can reuse the buffer.

/// @brief GC finalizer — frees the command buffer.
/// @param obj Opaque finalizing object expected to be a DebugDraw2D instance;
///            invalid handles are ignored.
static void debugdraw2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_DEBUGDRAW_CLASS_ID))
        return;
    rt_debugdraw2d_impl *debug_draw = (rt_debugdraw2d_impl *)obj;
    free(debug_draw->cmds);
}

/// @brief Allocate a DebugDraw2D with the given initial command-buffer capacity.
/// @details A nonpositive request selects the default capacity of 16, positive
///          requests up to 1,048,576 are preserved, and larger initial
///          requests are capped at that value. The queue starts empty.
/// @param capacity Requested number of command slots to allocate initially.
/// @return A new DebugDraw2D handle, or `NULL` if object or command-buffer
///         allocation fails.
void *rt_debugdraw2d_new(int64_t capacity) {
    rt_debugdraw2d_impl *debug_draw = (rt_debugdraw2d_impl *)rt_obj_new_i64(
        RT2D_DEBUGDRAW_CLASS_ID, (int64_t)sizeof(rt_debugdraw2d_impl));
    if (!debug_draw)
        return NULL;
    debug_draw->capacity = rt2d_initial_capacity(capacity);
    debug_draw->cmds =
        (rt_debugdraw2d_cmd *)calloc((size_t)debug_draw->capacity, sizeof(*debug_draw->cmds));
    if (!debug_draw->cmds) {
        if (rt_obj_release_check0(debug_draw))
            rt_obj_free(debug_draw);
        return NULL;
    }
    rt_obj_set_finalizer(debug_draw, debugdraw2d_finalize);
    return debug_draw;
}

/// @brief Discard all queued commands without freeing the backing buffer.
/// @param debug_draw Opaque DebugDraw2D handle to clear; invalid handles are
///                   ignored.
void rt_debugdraw2d_clear(void *debug_draw) {
    if (rt2d_has_class(debug_draw, RT2D_DEBUGDRAW_CLASS_ID))
        ((rt_debugdraw2d_impl *)debug_draw)->count = 0;
}

/// @brief Return the number of commands currently queued.
/// @param debug_draw Opaque DebugDraw2D handle to query.
/// @return The logical command count, or `0` when @p debug_draw is invalid.
int64_t rt_debugdraw2d_count(void *debug_draw) {
    return rt2d_has_class(debug_draw, RT2D_DEBUGDRAW_CLASS_ID)
               ? ((rt_debugdraw2d_impl *)debug_draw)->count
               : 0;
}

/// @brief Ensure the debug-draw command buffer has capacity for at least @p needed entries.
/// @details Grows geometrically from RT2D_INITIAL_CAP, doubling until capacity ≥ needed.
///          Guards against integer overflow when computing the byte size for
///          realloc and zero-initializes newly added slots. A null queue is
///          treated as already satisfied because callers reject it before
///          attempting an append.
/// @param debug_draw Mutable DebugDraw2D implementation whose buffer may be
///                   reallocated.
/// @param needed Minimum number of command slots required by the caller.
/// @return Nonzero for a null queue, sufficient existing capacity, or
///         successful growth; zero on overflow or allocation failure.
static int32_t debugdraw2d_reserve(rt_debugdraw2d_impl *debug_draw, int64_t needed) {
    if (!debug_draw || needed <= debug_draw->capacity)
        return 1;
    int64_t cap = debug_draw->capacity > 0 ? debug_draw->capacity : RT2D_INITIAL_CAP;
    while (cap < needed) {
        if (cap > INT64_MAX / 2)
            return 0;
        cap *= 2;
    }
    if (cap > INT64_MAX / (int64_t)sizeof(rt_debugdraw2d_cmd))
        return 0;
    rt_debugdraw2d_cmd *cmds =
        (rt_debugdraw2d_cmd *)realloc(debug_draw->cmds, (size_t)cap * sizeof(*cmds));
    if (!cmds)
        return 0;
    memset(cmds + debug_draw->capacity, 0, (size_t)(cap - debug_draw->capacity) * sizeof(*cmds));
    debug_draw->cmds = cmds;
    debug_draw->capacity = cap;
    return 1;
}

/// @brief Append a typed line, rectangle, or circle command to the queue.
/// @details Calls `debugdraw2d_reserve` to grow when needed. A reserve failure
///          raises a `DebugDraw2D: capacity overflow` runtime trap and leaves
///          the logical count unchanged. Coordinate fields have
///          shape-specific meanings described by `rt_debugdraw2d_cmd`.
/// @param debug_draw Mutable DebugDraw2D implementation; `NULL` is ignored.
/// @param type Primitive discriminator understood by the draw dispatcher.
/// @param x0 First X coordinate, or rectangle/circle X position.
/// @param y0 First Y coordinate, or rectangle/circle Y position.
/// @param x1 Second line X coordinate or rectangle width.
/// @param y1 Second line Y coordinate or rectangle height.
/// @param value Shape-specific auxiliary value, currently a circle radius.
/// @param rgba Packed RGB, packed RGBA, or tagged Color value stored for later
///             normalization.
static void debugdraw2d_add(rt_debugdraw2d_impl *debug_draw,
                            int32_t type,
                            int64_t x0,
                            int64_t y0,
                            int64_t x1,
                            int64_t y1,
                            int64_t value,
                            int64_t rgba) {
    if (!debug_draw)
        return;
    if (!debugdraw2d_reserve(debug_draw, debug_draw->count + 1)) {
        rt_trap("DebugDraw2D: capacity overflow");
        return;
    }
    rt_debugdraw2d_cmd *cmd = &debug_draw->cmds[debug_draw->count++];
    cmd->type = type;
    cmd->x0 = x0;
    cmd->y0 = y0;
    cmd->x1 = x1;
    cmd->y1 = y1;
    cmd->value = value;
    cmd->color = rgba;
}

/// @brief Queue a line from `(x0, y0)` to `(x1, y1)` with `rgba` color (type=1).
/// @param debug_draw Opaque DebugDraw2D handle to append to; invalid handles
///                   are ignored.
/// @param x0 Starting X coordinate.
/// @param y0 Starting Y coordinate.
/// @param x1 Ending X coordinate.
/// @param y1 Ending Y coordinate.
/// @param rgba Packed RGB, packed RGBA, or tagged Color value.
void rt_debugdraw2d_line(
    void *debug_draw, int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t rgba) {
    rt_debugdraw2d_impl *impl = rt2d_has_class(debug_draw, RT2D_DEBUGDRAW_CLASS_ID)
                                    ? (rt_debugdraw2d_impl *)debug_draw
                                    : NULL;
    debugdraw2d_add(impl, 1, x0, y0, x1, y1, 0, rgba);
}

/// @brief Queue a rectangle outline at `(x, y)` with `width × height` and `rgba` color (type=2).
/// @param debug_draw Opaque DebugDraw2D handle to append to; invalid handles
///                   are ignored.
/// @param x X coordinate of the rectangle origin.
/// @param y Y coordinate of the rectangle origin.
/// @param width Rectangle width stored without normalization.
/// @param height Rectangle height stored without normalization.
/// @param rgba Packed RGB, packed RGBA, or tagged Color value.
void rt_debugdraw2d_rect(
    void *debug_draw, int64_t x, int64_t y, int64_t width, int64_t height, int64_t rgba) {
    rt_debugdraw2d_impl *impl = rt2d_has_class(debug_draw, RT2D_DEBUGDRAW_CLASS_ID)
                                    ? (rt_debugdraw2d_impl *)debug_draw
                                    : NULL;
    debugdraw2d_add(impl, 2, x, y, width, height, 0, rgba);
}

/// @brief Queue a circle outline at `(x, y)` with the given `radius` and `rgba` color (type=3).
/// @param debug_draw Opaque DebugDraw2D handle to append to; invalid handles
///                   are ignored.
/// @param x X coordinate of the circle center.
/// @param y Y coordinate of the circle center.
/// @param radius Circle radius stored without normalization.
/// @param rgba Packed RGB, packed RGBA, or tagged Color value.
void rt_debugdraw2d_circle(void *debug_draw, int64_t x, int64_t y, int64_t radius, int64_t rgba) {
    rt_debugdraw2d_impl *impl = rt2d_has_class(debug_draw, RT2D_DEBUGDRAW_CLASS_ID)
                                    ? (rt_debugdraw2d_impl *)debug_draw
                                    : NULL;
    debugdraw2d_add(impl, 3, x, y, 0, 0, radius, rgba);
}

/// @brief Render all queued commands into `pixels` without consuming them.
/// @details Iterates the command list and dispatches to the appropriate
///          `rt_pixels_draw_*` primitive by command type:
///          - type 1 → `rt_pixels_draw_line`
///          - type 2 → `rt_pixels_draw_frame` (outline rect)
///          - type 3 → `rt_pixels_draw_ring` (outline circle)
///          Unknown types are silently skipped. Each stored color is normalized
///          to RGB and its alpha component is discarded.
/// @param debug_draw Opaque DebugDraw2D handle containing the retained queue.
/// @param pixels Borrowed destination Pixels handle modified in place.
void rt_debugdraw2d_draw_to_pixels(void *debug_draw, void *pixels) {
    rt_debugdraw2d_impl *impl = rt2d_has_class(debug_draw, RT2D_DEBUGDRAW_CLASS_ID)
                                    ? (rt_debugdraw2d_impl *)debug_draw
                                    : NULL;
    if (!impl || !pixels)
        return;
    for (int64_t i = 0; i < impl->count; i++) {
        rt_debugdraw2d_cmd *cmd = &impl->cmds[i];
        int64_t color = rt2d_draw_rgb(cmd->color);
        if (cmd->type == 1)
            rt_pixels_draw_line(pixels, cmd->x0, cmd->y0, cmd->x1, cmd->y1, color);
        else if (cmd->type == 2)
            rt_pixels_draw_frame(pixels, cmd->x0, cmd->y0, cmd->x1, cmd->y1, color);
        else if (cmd->type == 3)
            rt_pixels_draw_ring(pixels, cmd->x0, cmd->y0, cmd->value, color);
    }
}

//=============================================================================
