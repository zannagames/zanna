//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_pixels.h
/// @brief Declares the CPU-side Pixels image API for allocation, raw access,
///        image I/O, transforms, effects, drawing, and text.
///
/// @details Pixels store row-major numeric `0xRRGGBBAA` words. Scalar storage
/// APIs distinguish raw RGBA, Canvas-style RGB, and tagged Color values, while
/// drawing primitives accept RGB/tagged Color input and clip to image bounds.
/// Operations returning Pixels or Bytes create independent managed objects;
/// direct-buffer access is borrowed.
///
// File: src/runtime/graphics/2d/rt_pixels.h
// Purpose: Software image buffer manipulation for Zanna.Graphics.Pixels, providing pixel-level
// read/write, drawing primitives, image loading/saving, transforms, and rectangle copying.
//
// Key invariants:
//   - Pixel words use numeric 0xRRGGBBAA packing; drawing helpers use 0x00RRGGBB
//     or explicitly tagged Color values.
//   - Coordinates are 0-based from the top-left corner.
//   - Out-of-bounds reads return 0; out-of-bounds writes and drawing spans are clipped/no-op.
//   - Drawing primitives (box, disc, line, etc.) render directly into the pixel buffer.
//
// Ownership/Lifetime:
//   - Pixels objects are managed runtime objects exposed as opaque pointers.
//   - Newly returned objects carry their own reference; raw-buffer pointers are
//     borrowed and must not be freed.
//
// Links: src/runtime/graphics/2d/rt_pixels.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

/// Runtime class identifier assigned to Pixels object payloads.
#define RT_PIXELS_CLASS_ID INT64_C(-0x600201)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new Pixels buffer with given dimensions.
/// @param width Nonnegative width in pixels; zero is permitted.
/// @param height Nonnegative height in pixels; zero is permitted.
/// @return A new transparent-black Pixels object, or `NULL` for invalid
///         dimensions or allocation failure.
void *rt_pixels_new(int64_t width, int64_t height);

/// @brief Get the width of the Pixels buffer.
/// @param pixels Opaque Pixels handle.
/// @return The stored width, or `0` after invalid input.
int64_t rt_pixels_width(void *pixels);

/// @brief Get the height of the Pixels buffer.
/// @param pixels Opaque Pixels handle.
/// @return The stored height, or `0` after invalid input.
int64_t rt_pixels_height(void *pixels);

/// @brief Get a pixel color at (x, y) as raw packed RGBA.
/// @param pixels Pixels object.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @return Pixel color as packed RGBA (0xRRGGBBAA), or 0 if out of bounds.
int64_t rt_pixels_get(void *pixels, int64_t x, int64_t y);

/// @brief Get a pixel color at (x, y) as raw packed RGBA. Explicit alias for Pixels.GetRGBA.
/// @param pixels Opaque Pixels handle.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @return Raw `0xRRGGBBAA`, or transparent black when out of bounds.
int64_t rt_pixels_get_rgba(void *pixels, int64_t x, int64_t y);

/// @brief Get a pixel color at (x, y) as a Zanna.Graphics.Color-compatible value.
/// @param pixels Opaque Pixels handle.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @return The tagged Color representation; out-of-bounds reads convert
///         transparent black.
int64_t rt_pixels_get_color(void *pixels, int64_t x, int64_t y);

/// @brief Set a raw RGBA pixel at (x, y).
/// @param pixels Pixels object.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @param color Pixel color as packed RGBA (0xRRGGBBAA).
void rt_pixels_set(void *pixels, int64_t x, int64_t y, int64_t color);

/// @brief Set a raw RGBA pixel at (x, y). Explicit alias for Pixels.SetRGBA.
/// @param pixels Opaque destination Pixels handle.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @param rgba Raw `0xRRGGBBAA` or explicitly tagged Color value.
void rt_pixels_set_rgba(void *pixels, int64_t x, int64_t y, int64_t rgba);

/// @brief Set a pixel from a Canvas RGB or Color.RGBA value, converting to raw RGBA.
/// @param pixels Opaque destination Pixels handle.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @param color Canvas `0x00RRGGBB` or explicitly tagged Color value.
void rt_pixels_set_color(void *pixels, int64_t x, int64_t y, int64_t color);

/// @brief Blend bright texels toward a team color (luminance-masked tint).
/// @param pixels Borrowed Pixels handle mutated in place.
/// @param rgb Packed 0xRRGGBB target color.
/// @param strength Blend strength at full mask, clamped to [0, 1].
/// @param lum_lo Luma where the mask begins (0..255).
/// @param lum_hi Luma where the mask reaches full strength (> lum_lo).
void rt_pixels_tint_luminance_masked(
    void *pixels, int64_t rgb, double strength, int64_t lum_lo, int64_t lum_hi);

/// @brief Recolor texels near a reference color toward a target color,
///        preserving per-texel shading (see rt_pixels_transform.c).
/// @param target_rgb Replacement color 0xRRGGBB.
/// @param ref_rgb Reference color class 0xRRGGBB.
/// @param tolerance Euclidean RGB radius of the class (full inside /2).
void rt_pixels_tint_masked_neutral(
    void *pixels, void *mask, int64_t rgb, double strength, int64_t lum_lo, int64_t lum_hi,
    int64_t neutral_max);
void rt_pixels_recolor_masked(
    void *pixels, int64_t target_rgb, int64_t ref_rgb, int64_t tolerance);

/// @brief Grow covered texels into uncovered neighbors (UV-atlas gutter fill).
/// @details Each pass assigns every uncovered texel with at least one covered
///   8-neighbor the average color of those covered neighbors, then marks it
///   covered. Coverage comes from @p mask (any non-zero RGB = covered) and the
///   mask is updated in place as coverage grows. Mip generation on an atlas
///   whose gutters keep the background color bleeds that background into every
///   island edge at minification; dilating island colors outward first makes
///   the mip averages stay island-colored.
/// @param pixels Atlas to dilate in place (0xRRGGBBAA texels).
/// @param mask Coverage mask; must match @p pixels dimensions exactly.
/// @param passes Dilation ring width in texels (clamped to [0, 256]).
void rt_pixels_dilate_masked(void *pixels, void *mask, int64_t passes);

/// @brief Grow covered texels into uncovered neighbors by EXACT owner copy.
/// @details Like rt_pixels_dilate_masked, but each claimed texel copies the
///   exact RGBA of its first covered 8-neighbor in the fixed (dy,dx) scan
///   order (-1,-1)..(1,1) instead of averaging — values never decay, so a
///   binary label map and its atlas share one deterministic watershed
///   topology (the averaging op truncates a 255-vs-0 label front to zero
///   within a few rings). Ring-synchronous: a texel claimed in a ring never
///   feeds another claim in the same ring.
/// @param pixels Atlas or label map to grow in place (0xRRGGBBAA texels).
/// @param mask Coverage mask; must match @p pixels dimensions exactly;
///   updated in place (claimed texels stamp 0xFFFFFFFF).
/// @param passes Ring count; <= 0 runs to convergence (full-surface fill).
void rt_pixels_dilate_owner(void *pixels, void *mask, int64_t passes);

/// @brief Mask-scoped shade-preserving colorize with an explicit reference
///   luminance and shade clamp.
/// @details rt_pixels_recolor_masked's interior formula with the color-class
///   gates replaced by an explicit coverage mask: for each texel covered by
///   @p mask (any non-zero RGB at the proportionally scaled coordinate),
///   `shade = lum / ref_lum` (clamped to @p max_shade) and the texel blends
///   toward `target * shade` by @p strength. Alpha untouched. Dark authored
///   regions (a navy cap, ref_lum ~13) reach any bright target at full
///   brightness — the fixed ref/1.5-clamp op crushed them near black.
/// @param pixels Borrowed Pixels handle mutated in place.
/// @param mask Coverage mask; any size (coordinates scale proportionally).
/// @param rgb Packed 0xRRGGBB target color.
/// @param ref_lum Reference luminance mapping to `target` exactly (>= 1).
/// @param max_shade Upper clamp for the shade ratio (invalid/<=0 -> 1.5).
/// @param strength Blend strength, clamped to [0, 1].
void rt_pixels_colorize_masked(void *pixels, void *mask, int64_t rgb, int64_t ref_lum,
                               double max_shade, double strength);

/// @brief Linear-light variant of rt_pixels_colorize_masked (ADR 0293):
///   identical signature and gating, but the shade transfer runs in linear
///   light through the exact sRGB EOTF the 3D backends apply to albedo
///   textures, so a recolored PBR region is no longer double-darkened by
///   the shader's linearization. Display-referred rasters keep the
///   byte-space op.
void rt_pixels_colorize_masked_linear(void *pixels, void *mask, int64_t rgb, int64_t ref_lum,
                                      double max_shade, double strength);

/// @brief Copy every texel of @p src whose RGB is non-zero over the
///   receiver (same dimensions; mismatch is a no-op). The sparse-layer
///   stamp: offline tools bake per-texel patches (e.g. garment-sourced
///   fills for the AI bakes' black occlusion holes) into a mostly-zero
///   layer, and the runtime applies them in one native pass.
void rt_pixels_stamp_nonzero(void *pixels, void *src);

/// @brief Get direct read-only access to the underlying RGBA pixel buffer.
/// @param pixels Pixels object.
/// @return Pointer to width*height uint32_t values (0xRRGGBBAA), or NULL.
/// @warning The buffer length is width*height — query rt_pixels_width()/height()
///   for bounds; no length is returned here. The pointer is embedded in and
///   valid only for the lifetime of this exact Pixels object. Do not free it or
///   retain it after releasing the object.
const uint32_t *rt_pixels_raw_buffer(void *pixels);

/// @brief Return the mutation generation for cache invalidation.
/// @param pixels Opaque Pixels handle.
/// @return The current generation, or `0` for invalid input.
uint64_t rt_pixels_generation(void *pixels);

/// @brief Fill entire buffer with a raw RGBA color.
/// @param pixels Pixels object.
/// @param color Fill color as packed RGBA (0xRRGGBBAA).
void rt_pixels_fill(void *pixels, int64_t color);

/// @brief Fill entire buffer with a raw RGBA color. Explicit alias for Pixels.Fill.
/// @param pixels Opaque destination Pixels handle.
/// @param rgba Raw `0xRRGGBBAA` or explicitly tagged Color value.
void rt_pixels_fill_rgba(void *pixels, int64_t rgba);

/// @brief Fill entire buffer from a Canvas RGB or Color.RGBA value, converting to raw RGBA.
/// @param pixels Opaque destination Pixels handle.
/// @param color Canvas `0x00RRGGBB` or explicitly tagged Color value.
void rt_pixels_fill_color(void *pixels, int64_t color);

/// @brief Clear buffer to transparent black (0x00000000).
/// @param pixels Opaque destination Pixels handle.
void rt_pixels_clear(void *pixels);

/// @brief Copy a rectangle from source to destination.
/// @param dst Destination Pixels object.
/// @param dx Destination X coordinate.
/// @param dy Destination Y coordinate.
/// @param src Source Pixels object.
/// @param sx Source X coordinate.
/// @param sy Source Y coordinate.
/// @param w Width of rectangle to copy.
/// @param h Height of rectangle to copy.
void rt_pixels_copy(
    void *dst, int64_t dx, int64_t dy, void *src, int64_t sx, int64_t sy, int64_t w, int64_t h);

/// @brief Create a deep copy of a Pixels buffer.
/// @param pixels Opaque source Pixels handle.
/// @return A new independent Pixels object with a distinct cache identity, or
///         `NULL` on failure.
void *rt_pixels_clone(void *pixels);

/// @brief Convert Pixels to raw bytes (RGBA, row-major).
/// @param pixels Opaque source Pixels handle.
/// @return A Zanna.Collections.Bytes object containing the pixel data.
void *rt_pixels_to_bytes(void *pixels);

/// @brief Create Pixels from raw bytes.
/// @param width Nonnegative width in pixels.
/// @param height Nonnegative height in pixels.
/// @param bytes Zanna.Collections.Bytes object containing RGBA data.
/// @return New Pixels object, or NULL if dimensions or bytes are invalid,
///         insufficient, or allocation fails.
void *rt_pixels_from_bytes(int64_t width, int64_t height, void *bytes);

//=========================================================================
// BMP Image I/O
//=========================================================================

/// @brief Load a BMP image from file.
/// @param path File path (runtime string).
/// @return New Pixels object, or NULL on failure.
/// @note Supports 24-bit uncompressed BMP files.
void *rt_pixels_load_bmp(void *path);

/// @brief Save a Pixels buffer to a BMP file.
/// @param pixels Pixels object to save.
/// @param path File path (runtime string).
/// @return 1 on success, 0 on failure.
int64_t rt_pixels_save_bmp(void *pixels, void *path);

//=========================================================================
// PNG Image I/O
//=========================================================================

/// @brief Load a PNG image from file.
/// @param path File path (runtime string).
/// @return New Pixels object, or NULL on failure.
/// @note Supports all PNG color types and bit depths.
void *rt_pixels_load_png(void *path);

/// @brief Decode a PNG memory buffer into malloc-owned raw RGBA32 pixels.
/// @details Internal worker-safe helper. The returned buffer stores 0xRRGGBBAA
///          pixels in row-major order; caller frees it with free().
/// @param data Borrowed encoded PNG byte buffer.
/// @param len Encoded buffer length in bytes.
/// @param out_pixels Receives the malloc-owned pixel array on success.
/// @param out_width Receives the decoded positive width.
/// @param out_height Receives the decoded positive height.
/// @return 1 on success, 0 on failure.
int rt_png_decode_buffer_rgba32(const uint8_t *data,
                                size_t len,
                                uint32_t **out_pixels,
                                int64_t *out_width,
                                int64_t *out_height);

/// @brief Load a JPEG image from a file path.
/// @param path File path (runtime string).
/// @return New Pixels object, or NULL on failure.
/// @note Supports baseline DCT JPEG: 8-bit, YCbCr/grayscale, 4:4:4/4:2:0/4:2:2.
void *rt_pixels_load_jpeg(void *path);

/// @brief Decode a JPEG image from a memory buffer.
/// @param data Pointer to JPEG data (must start with 0xFFD8 SOI marker).
/// @param len Length of data in bytes.
/// @return New Pixels object, or NULL on failure. Does NOT take ownership of data.
void *rt_jpeg_decode_buffer(const uint8_t *data, size_t len);

/// @brief Decode a JPEG memory buffer into malloc-owned raw RGBA32 pixels.
/// @details Internal worker-safe helper. The returned buffer stores 0xRRGGBBAA
///          pixels in row-major order; caller frees it with free().
/// @param data Borrowed encoded JPEG byte buffer.
/// @param len Encoded buffer length in bytes.
/// @param out_pixels Receives the malloc-owned pixel array on success.
/// @param out_width Receives the decoded positive width.
/// @param out_height Receives the decoded positive height.
/// @return 1 on success, 0 on failure.
int rt_jpeg_decode_buffer_rgba32(const uint8_t *data,
                                 size_t len,
                                 uint32_t **out_pixels,
                                 int64_t *out_width,
                                 int64_t *out_height);

/// @brief Load a GIF image from a file path (first frame only for static use).
/// @param path File path (runtime string).
/// @return New Pixels object (first frame), or NULL on failure.
/// @note For animated GIFs, use Sprite.FromFile() which loads all frames.
void *rt_pixels_load_gif(void *path);

/// @brief Load an image from a file path, auto-detecting format from magic bytes.
/// @details Supports PNG, JPEG, BMP, and GIF. Returns first frame for animated GIFs.
/// @param path File path (runtime string).
/// @return New Pixels object, or NULL on failure.
void *rt_pixels_load(void *path);

/// @brief Save a Pixels buffer to a PNG file.
/// @param pixels Pixels object to save.
/// @param path File path (runtime string).
/// @return 1 on success, 0 on failure.
int64_t rt_pixels_save_png(void *pixels, void *path);

//=========================================================================
// Image Transforms
//=========================================================================

/// @brief Flip the image horizontally (mirror left-right).
/// @param pixels Pixels object.
/// @return New flipped Pixels object.
void *rt_pixels_flip_h(void *pixels);

/// @brief Flip the image vertically (mirror top-bottom).
/// @param pixels Pixels object.
/// @return New flipped Pixels object.
void *rt_pixels_flip_v(void *pixels);

/// @brief Rotate the image 90 degrees clockwise.
/// @param pixels Pixels object.
/// @return New rotated Pixels object (width and height swapped).
void *rt_pixels_rotate_cw(void *pixels);

/// @brief Rotate the image 90 degrees counter-clockwise.
/// @param pixels Pixels object.
/// @return New rotated Pixels object (width and height swapped).
void *rt_pixels_rotate_ccw(void *pixels);

/// @brief Rotate the image 180 degrees.
/// @param pixels Pixels object.
/// @return New rotated Pixels object.
void *rt_pixels_rotate_180(void *pixels);

/// @brief Rotate the image by an arbitrary angle.
/// @param pixels Pixels object.
/// @param angle_degrees Rotation angle in degrees (positive = clockwise).
/// @return New rotated Pixels object with expanded dimensions to fit.
/// @note Uses bilinear interpolation for smooth results.
void *rt_pixels_rotate(void *pixels, double angle_degrees);

/// @brief Scale the image using nearest-neighbor interpolation.
/// @param pixels Pixels object.
/// @param new_width Target width.
/// @param new_height Target height.
/// @return New scaled Pixels object.
void *rt_pixels_scale(void *pixels, int64_t new_width, int64_t new_height);

//=========================================================================
// Image Processing
//=========================================================================

/// @brief Invert all colors in the image.
/// @param pixels Pixels object.
/// @return New inverted Pixels object.
void *rt_pixels_invert(void *pixels);

/// @brief Convert image to grayscale.
/// @param pixels Pixels object.
/// @return New grayscale Pixels object.
void *rt_pixels_grayscale(void *pixels);

/// @brief Apply a color tint to the image.
/// @param pixels Pixels object.
/// @param color Tint color (0x00RRGGBB).
/// @return New tinted Pixels object.
void *rt_pixels_tint(void *pixels, int64_t color);

/// @brief Apply a box blur to the image.
/// @param pixels Pixels object.
/// @param radius Blur radius (0 returns an exact copy; positive values are clamped to 10).
/// @return New blurred Pixels object.
void *rt_pixels_blur(void *pixels, int64_t radius);

/// @brief Scale the image using bilinear interpolation.
/// @param pixels Pixels object.
/// @param new_width Target width.
/// @param new_height Target height.
/// @return New scaled Pixels object.
void *rt_pixels_resize(void *pixels, int64_t new_width, int64_t new_height);

//=========================================================================
// Drawing Primitives
//=========================================================================
// Drawing primitives accept Canvas 0x00RRGGBB, Color.RGB(), and tagged
// Color.RGBA() values. RGB-only inputs draw with alpha 255; tagged RGBA
// values preserve alpha. Coordinates outside the buffer are silently clipped.

/// @brief Set a pixel using 0x00RRGGBB color format (alpha = 255).
/// @param pixels Opaque destination Pixels handle.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_set_rgb(void *pixels, int64_t x, int64_t y, int64_t color);

/// @brief Get a pixel as 0x00RRGGBB (alpha channel discarded).
/// @param pixels Opaque Pixels handle.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @return Low-24-bit RGB, or `0` for invalid/out-of-bounds input.
int64_t rt_pixels_get_rgb(void *pixels, int64_t x, int64_t y);

/// @brief Draw a line between two points (Bresenham algorithm).
/// @param pixels Opaque destination Pixels handle.
/// @param x1 Start X.
/// @param y1 Start Y.
/// @param x2 End X.
/// @param y2 End Y.
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_draw_line(
    void *pixels, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color);

/// @brief Draw a filled rectangle.
/// @param pixels Opaque destination Pixels handle.
/// @param x Rectangle origin X.
/// @param y Rectangle origin Y.
/// @param w Requested width.
/// @param h Requested height.
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_draw_box(void *pixels, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color);

/// @brief Draw a rectangle outline.
/// @param pixels Opaque destination Pixels handle.
/// @param x Rectangle origin X.
/// @param y Rectangle origin Y.
/// @param w Requested width.
/// @param h Requested height.
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_draw_frame(void *pixels, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color);

/// @brief Draw a filled circle.
/// @param pixels Opaque destination Pixels handle.
/// @param cx Center X.
/// @param cy Center Y.
/// @param r Radius.
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_draw_disc(void *pixels, int64_t cx, int64_t cy, int64_t r, int64_t color);

/// @brief Draw a circle outline.
/// @param pixels Opaque destination Pixels handle.
/// @param cx Center X.
/// @param cy Center Y.
/// @param r Radius.
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_draw_ring(void *pixels, int64_t cx, int64_t cy, int64_t r, int64_t color);

/// @brief Draw a filled ellipse.
/// @param pixels Opaque destination Pixels handle.
/// @param cx Center X.
/// @param cy Center Y.
/// @param rx Horizontal radius.
/// @param ry Vertical radius.
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_draw_ellipse(
    void *pixels, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color);

/// @brief Draw an ellipse outline.
/// @param pixels Opaque destination Pixels handle.
/// @param cx Center X.
/// @param cy Center Y.
/// @param rx Horizontal radius.
/// @param ry Vertical radius.
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_draw_ellipse_frame(
    void *pixels, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color);

/// @brief Flood fill from a seed point (iterative scanline, any canvas size).
/// @param pixels Opaque destination Pixels handle.
/// @param x Seed X.
/// @param y Seed Y.
/// @param color Replacement Canvas RGB or tagged Color value.
void rt_pixels_flood_fill(void *pixels, int64_t x, int64_t y, int64_t color);

/// @brief Draw a thick line (pen-radius approach).
/// @param pixels Opaque destination Pixels handle.
/// @param x1 Start X.
/// @param y1 Start Y.
/// @param x2 End X.
/// @param y2 End Y.
/// @param thickness Stroke width in pixels (pen diameter).
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_draw_thick_line(
    void *pixels, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t thickness, int64_t color);

/// @brief Draw a filled triangle (scanline fill).
/// @param pixels Opaque destination Pixels handle.
/// @param x1 First vertex X.
/// @param y1 First vertex Y.
/// @param x2 Second vertex X.
/// @param y2 Second vertex Y.
/// @param x3 Third vertex X.
/// @param y3 Third vertex Y.
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_draw_triangle(void *pixels,
                             int64_t x1,
                             int64_t y1,
                             int64_t x2,
                             int64_t y2,
                             int64_t x3,
                             int64_t y3,
                             int64_t color);

/// @brief Draw a quadratic Bézier curve.
/// @param pixels Opaque destination Pixels handle.
/// @param x1 Start X.
/// @param y1 Start Y.
/// @param cx Control-point X.
/// @param cy Control-point Y.
/// @param x2 End X.
/// @param y2 End Y.
/// @param color Canvas RGB or tagged Color value.
void rt_pixels_draw_bezier(void *pixels,
                           int64_t x1,
                           int64_t y1,
                           int64_t cx,
                           int64_t cy,
                           int64_t x2,
                           int64_t y2,
                           int64_t color);

/// @brief Draw built-in 8x8 bitmap-font text at (x, y).
/// @param pixels Opaque destination Pixels handle.
/// @param x Text origin X.
/// @param y Text origin Y.
/// @param text Runtime string to render.
/// @param color Foreground Canvas RGB or tagged Color value.
void rt_pixels_draw_text(void *pixels, int64_t x, int64_t y, rt_string text, int64_t color);

/// @brief Draw built-in 8x8 bitmap-font text with a filled background cell per glyph pixel.
/// @param pixels Opaque destination Pixels handle.
/// @param x Text origin X.
/// @param y Text origin Y.
/// @param text Runtime string to render.
/// @param fg Foreground Canvas RGB or tagged Color value.
/// @param bg Background Canvas RGB or tagged Color value.
void rt_pixels_draw_text_bg(
    void *pixels, int64_t x, int64_t y, rt_string text, int64_t fg, int64_t bg);

/// @brief Return rendered text width in pixels at 1x scale.
/// @param text Runtime string to measure.
/// @return Rendered width in pixels.
int64_t rt_pixels_text_width(rt_string text);

/// @brief Return built-in font line height in pixels.
/// @return The fixed built-in font line height.
int64_t rt_pixels_text_height(void);

/// @brief Draw built-in text scaled by an integer factor.
/// @param pixels Opaque destination Pixels handle.
/// @param x Text origin X.
/// @param y Text origin Y.
/// @param text Runtime string to render.
/// @param scale Requested integer scale.
/// @param color Foreground Canvas RGB or tagged Color value.
void rt_pixels_draw_text_scaled(
    void *pixels, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t color);

/// @brief Draw scaled built-in text with a filled background cell per glyph pixel.
/// @param pixels Opaque destination Pixels handle.
/// @param x Text origin X.
/// @param y Text origin Y.
/// @param text Runtime string to render.
/// @param scale Requested integer scale.
/// @param fg Foreground Canvas RGB or tagged Color value.
/// @param bg Background Canvas RGB or tagged Color value.
void rt_pixels_draw_text_scaled_bg(
    void *pixels, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t fg, int64_t bg);

/// @brief Return rendered text width in pixels at the given integer scale.
/// @param text Runtime string to measure.
/// @param scale Requested integer scale.
/// @return Rendered width in pixels.
int64_t rt_pixels_text_scaled_width(rt_string text, int64_t scale);

/// @brief Draw text horizontally centered in the Pixels buffer at row y.
/// @param pixels Opaque destination Pixels handle.
/// @param y Text origin Y.
/// @param text Runtime string to render.
/// @param color Foreground Canvas RGB or tagged Color value.
void rt_pixels_draw_text_centered(void *pixels, int64_t y, rt_string text, int64_t color);

/// @brief Draw text right-aligned to the Pixels buffer with the given margin.
/// @param pixels Opaque destination Pixels handle.
/// @param margin Distance from the image's right edge.
/// @param y Text origin Y.
/// @param text Runtime string to render.
/// @param color Foreground Canvas RGB or tagged Color value.
void rt_pixels_draw_text_right(
    void *pixels, int64_t margin, int64_t y, rt_string text, int64_t color);

/// @brief Draw scaled text horizontally centered in the Pixels buffer at row y.
/// @param pixels Opaque destination Pixels handle.
/// @param y Text origin Y.
/// @param text Runtime string to render.
/// @param color Foreground Canvas RGB or tagged Color value.
/// @param scale Requested integer scale.
void rt_pixels_draw_text_centered_scaled(
    void *pixels, int64_t y, rt_string text, int64_t color, int64_t scale);

/// @brief Alpha-composite a color onto a pixel (Porter-Duff over).
/// @param pixels Pixels object.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @param color Source color in 0x00RRGGBB format (Canvas-compatible).
/// @param alpha Source alpha 0–255 (0 = transparent, 255 = fully opaque).
/// @note Coordinates outside the buffer are silently clipped.
void rt_pixels_blend_pixel(void *pixels, int64_t x, int64_t y, int64_t color, int64_t alpha);

#ifdef __cplusplus
}
#endif
