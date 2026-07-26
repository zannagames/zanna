//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_image.c
// Purpose: Image widget implementation — atomically stores and incrementally
//          updates RGBA pixels, caches deterministic resized output, and renders
//          with configurable scale modes, filters, and opacity.
// Key invariants:
//   - Stored pixels are always 32-bit RGBA (4 bytes per pixel), unpremultiplied.
//   - VG_IMAGE_SCALE_NONE draws at the image's natural size from widget origin.
//   - VG_IMAGE_SCALE_FIT scales uniformly to fit inside the widget bounds.
//   - VG_IMAGE_SCALE_FILL scales uniformly to fill, cropping excess pixels.
//   - opacity is clamped to [0.0, 1.0]; 0.0 = fully transparent, 1.0 = opaque.
//   - paint respects the canvas clip rectangle reported by ZannaGFX.
//   - Failed validation/allocation never replaces or partially mutates pixels.
//   - Nearest and bilinear resize output is deterministic and cacheable.
// Ownership/Lifetime:
//   - Source and scaled buffers are owned by the image and freed by destroy;
//     caller-supplied buffers are copied and never retained.
// Links: lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_widget.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements the retained RGBA image widget, image decoding, resizing,
///        filtering, and framebuffer compositing.
/// @details Image content is copied into widget-owned straight-alpha RGBA
///          storage. Resized rasters are cached by content revision and filter,
///          while invalid input or allocation failure leaves existing content
///          intact. The paint path clips all writes to both the widget and
///          canvas clip rectangles.
#include "../../include/vg_theme.h"
#include "../../include/vg_widget.h"
#include "../../include/vg_widgets.h"
#include "../vg_file_stdio.h"
#include "vgfx.h"
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void image_destroy(vg_widget_t *widget);
static void image_measure(vg_widget_t *widget, float available_width, float available_height);
static void image_paint(vg_widget_t *widget, void *canvas);
static bool image_can_focus(vg_widget_t *widget);

static vg_widget_vtable_t g_image_vtable = {
    .destroy = image_destroy,
    .measure = image_measure,
    .arrange = NULL,
    .paint = image_paint,
    .handle_event = NULL,
    .can_focus = image_can_focus,
};

/// @brief Focus participation is opt-in: only focusable-flagged images (scene
///        canvases) join click-to-focus and keyboard traversal.
/// @param widget Image widget whose focus policy and current state are queried.
/// @return True when the image opted into focus and is both enabled and visible.
static bool image_can_focus(vg_widget_t *widget) {
    const vg_image_t *image = (const vg_image_t *)widget;
    return image->focusable && widget->enabled && widget->visible;
}

/// @brief Restrict an integer to an inclusive range.
/// @param value Value to restrict.
/// @param min_value Inclusive lower bound.
/// @param max_value Inclusive upper bound.
/// @return `min_value` or `max_value` when @p value is outside the range;
///         otherwise @p value unchanged.
static int clampi(int value, int min_value, int max_value) {
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

/// @brief Validate RGBA dimensions and calculate their required byte count.
/// @param width Positive image width.
/// @param height Positive image height.
/// @param out_size Receives width times height times four on success.
/// @return true when the dimensions are positive and all arithmetic fits size_t.
static bool image_rgba_size(int width, int height, size_t *out_size) {
    if (width <= 0 || height <= 0 || !out_size)
        return false;
    const size_t w = (size_t)width;
    const size_t h = (size_t)height;
    if (w > SIZE_MAX / h || w * h > SIZE_MAX / 4u)
        return false;
    *out_size = w * h * 4u;
    return true;
}

/// @brief Advance a saturating unsigned revision counter.
/// @param revision Counter to update; NULL is ignored.
static void image_revision_advance(uint64_t *revision) {
    if (revision && *revision != UINT64_MAX)
        ++*revision;
}

/// @brief Test whether all pixels in an RGBA buffer have full alpha.
/// @param pixels RGBA source bytes.
/// @param byte_count Total byte count, which must be divisible by four.
/// @return true for an empty buffer or when every alpha byte equals 255.
static bool image_pixels_are_opaque(const uint8_t *pixels, size_t byte_count) {
    if (!pixels)
        return byte_count == 0;
    for (size_t offset = 3u; offset < byte_count; offset += 4u) {
        if (pixels[offset] != 255u)
            return false;
    }
    return true;
}

/// @brief Invalidate cached resize metadata without releasing reusable storage.
/// @param image Image whose next resized paint must regenerate cached pixels.
static void image_invalidate_scaled_cache(vg_image_t *image) {
    if (!image)
        return;
    image->scaled_width = 0;
    image->scaled_height = 0;
    image->scaled_revision = 0;
}

/// @brief Record a source-content mutation and schedule the required retained work.
/// @param image Mutated image.
/// @param layout_changed true when intrinsic dimensions changed and layout must rerun.
static void image_note_content_change(vg_image_t *image, bool layout_changed) {
    if (!image)
        return;
    image_revision_advance(&image->content_revision);
    image_invalidate_scaled_cache(image);
    vg_widget_note_revision(&image->base);
    if (layout_changed)
        vg_widget_invalidate_layout(&image->base);
    else
        vg_widget_invalidate(&image->base);
}

/// @brief Interpolate two channel values with an unsigned 16.16 fraction.
/// @param a Channel value at the lower coordinate.
/// @param b Channel value at the upper coordinate.
/// @param fraction Fraction in the inclusive range 0..65535.
/// @return Rounded interpolated channel value.
static uint8_t image_lerp_channel(uint8_t a, uint8_t b, uint32_t fraction) {
    const uint32_t inverse = 65536u - fraction;
    return (uint8_t)(((uint32_t)a * inverse + (uint32_t)b * fraction + 32768u) >> 16u);
}

/// @brief Map a destination pixel center to a clamped 16.16 source coordinate.
/// @details Quotient/remainder decomposition avoids overflowing a shifted 64-bit numerator and is
///          portable to compilers without a 128-bit integer extension.
/// @param position Zero-based destination coordinate.
/// @param source_extent Positive source dimension.
/// @param dest_extent Positive destination dimension.
/// @return Source coordinate in unsigned 16.16 units, clamped to the final source pixel.
static int64_t image_center_coordinate_16(int position, int source_extent, int dest_extent) {
    const int64_t numerator = (int64_t)(2 * (int64_t)position + 1) * source_extent;
    const int64_t denominator = 2 * (int64_t)dest_extent;
    const int64_t whole = numerator / denominator;
    const int64_t remainder = numerator % denominator;
    int64_t coordinate = whole * 65536 + (remainder * 65536) / denominator - 32768;
    const int64_t maximum = (int64_t)(source_extent - 1) * 65536;
    if (coordinate < 0)
        coordinate = 0;
    if (coordinate > maximum)
        coordinate = maximum;
    return coordinate;
}

/// @brief Sample one resized source pixel with the image's active filter.
/// @param image Image containing valid source pixels.
/// @param dest_x Zero-based coordinate in the resized output.
/// @param dest_y Zero-based coordinate in the resized output.
/// @param dest_width Positive resized width.
/// @param dest_height Positive resized height.
/// @param out_rgba Four-byte destination receiving straight RGBA.
static void image_sample_resized(const vg_image_t *image,
                                 int dest_x,
                                 int dest_y,
                                 int dest_width,
                                 int dest_height,
                                 uint8_t out_rgba[4]) {
    const int source_width = image->img_width;
    const int source_height = image->img_height;
    if (image->filter == VG_IMAGE_FILTER_NEAREST) {
        const int source_x =
            clampi((int)(((int64_t)dest_x * source_width) / dest_width), 0, source_width - 1);
        const int source_y =
            clampi((int)(((int64_t)dest_y * source_height) / dest_height), 0, source_height - 1);
        memcpy(out_rgba,
               image->pixels + ((size_t)source_y * (size_t)source_width + (size_t)source_x) * 4u,
               4u);
        return;
    }

    const int64_t source_x_16 = image_center_coordinate_16(dest_x, source_width, dest_width);
    const int64_t source_y_16 = image_center_coordinate_16(dest_y, source_height, dest_height);
    const int x0 = (int)(source_x_16 >> 16u);
    const int y0 = (int)(source_y_16 >> 16u);
    const int x1 = x0 + 1 < source_width ? x0 + 1 : x0;
    const int y1 = y0 + 1 < source_height ? y0 + 1 : y0;
    const uint32_t fraction_x = (uint32_t)(source_x_16 & 0xFFFF);
    const uint32_t fraction_y = (uint32_t)(source_y_16 & 0xFFFF);
    const uint8_t *p00 = image->pixels + ((size_t)y0 * (size_t)source_width + (size_t)x0) * 4u;
    const uint8_t *p10 = image->pixels + ((size_t)y0 * (size_t)source_width + (size_t)x1) * 4u;
    const uint8_t *p01 = image->pixels + ((size_t)y1 * (size_t)source_width + (size_t)x0) * 4u;
    const uint8_t *p11 = image->pixels + ((size_t)y1 * (size_t)source_width + (size_t)x1) * 4u;
    for (size_t channel = 0; channel < 4u; ++channel) {
        const uint8_t top = image_lerp_channel(p00[channel], p10[channel], fraction_x);
        const uint8_t bottom = image_lerp_channel(p01[channel], p11[channel], fraction_x);
        out_rgba[channel] = image_lerp_channel(top, bottom, fraction_y);
    }
}

/// @brief Materialize or reuse the deterministic resized-pixel cache.
/// @details Allocation failure is non-fatal; callers can fall back to direct sampling. The prior
///          cache remains owned and valid until a replacement allocation succeeds.
/// @param image Image with valid source pixels.
/// @param width Positive target width.
/// @param height Positive target height.
/// @return Borrowed cache pointer, or NULL if dimensions are invalid or allocation failed.
static const uint8_t *image_scaled_cache(vg_image_t *image, int width, int height) {
    size_t required = 0;
    if (!image || !image->pixels || !image_rgba_size(width, height, &required))
        return NULL;
    if (image->scaled_pixels && image->scaled_width == width && image->scaled_height == height &&
        image->scaled_revision == image->content_revision &&
        image->scaled_filter == image->filter) {
        return image->scaled_pixels;
    }

    if (required > image->scaled_capacity) {
        uint8_t *replacement = (uint8_t *)malloc(required);
        if (!replacement)
            return NULL;
        free(image->scaled_pixels);
        image->scaled_pixels = replacement;
        image->scaled_capacity = required;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image_sample_resized(image,
                                 x,
                                 y,
                                 width,
                                 height,
                                 image->scaled_pixels +
                                     ((size_t)y * (size_t)width + (size_t)x) * 4u);
        }
    }
    image->scaled_width = width;
    image->scaled_height = height;
    image->scaled_revision = image->content_revision;
    image->scaled_filter = image->filter;
    return image->scaled_pixels;
}

/// @brief Decode an unaligned two-byte little-endian integer.
/// @param p Pointer to at least two readable bytes.
/// @return Decoded unsigned 16-bit value.
static uint16_t image_read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/// @brief Decode an unaligned four-byte little-endian integer.
/// @param p Pointer to at least four readable bytes.
/// @return Decoded unsigned 32-bit value.
static uint32_t image_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// @brief Convert premultiplied RGBA pixels in place to straight alpha.
/// @details Fully transparent pixels are normalized to transparent black.
///          Partially transparent color channels are divided by alpha and
///          saturated to the byte range.
/// @param pixels Mutable buffer containing @p width times @p height RGBA pixels;
///               `NULL` is ignored.
/// @param width Image width in pixels.
/// @param height Image height in pixels.
static void image_unpremultiply_rgba(uint8_t *pixels, size_t width, size_t height) {
    if (!pixels || width == 0 || height == 0 || width > SIZE_MAX / height)
        return;

    size_t pixel_count = width * height;
    for (size_t i = 0; i < pixel_count; i++) {
        uint8_t *p = pixels + i * 4u;
        uint8_t a = p[3];
        if (a == 0) {
            p[0] = 0;
            p[1] = 0;
            p[2] = 0;
        } else if (a < 255) {
            uint32_t r = (uint32_t)p[0] * 255u / (uint32_t)a;
            uint32_t g = (uint32_t)p[1] * 255u / (uint32_t)a;
            uint32_t b = (uint32_t)p[2] * 255u / (uint32_t)a;
            p[0] = (uint8_t)(r > 255u ? 255u : r);
            p[1] = (uint8_t)(g > 255u ? 255u : g);
            p[2] = (uint8_t)(b > 255u ? 255u : b);
        }
    }
}

/// @brief Decode an uncompressed 24- or 32-bit BMP into an image widget.
/// @details Accepts bottom-up and top-down Windows bitmap layouts, converts BGR
///          or BGRA rows to straight RGBA, and commits the complete result
///          atomically through vg_image_try_set_pixels().
/// @param image Image widget that receives a copy of the decoded pixels.
/// @param path Null-terminated path to the BMP file.
/// @return `true` when the file was valid and its complete pixel array was
///         installed; `false` on I/O, validation, or allocation failure.
static bool image_load_bmp(vg_image_t *image, const char *path) {
    FILE *f = vg_file_open_read_utf8(path);
    if (!f)
        return false;

    bool ok = false;
    uint8_t header[54];
    uint8_t *row = NULL;
    uint8_t *rgba = NULL;

    if (fread(header, 1, sizeof(header), f) != sizeof(header))
        goto cleanup;
    if (header[0] != 'B' || header[1] != 'M')
        goto cleanup;

    uint32_t pixel_offset = image_read_le32(header + 10);
    uint32_t dib_size = image_read_le32(header + 14);
    if (dib_size < 40)
        goto cleanup;
    if (dib_size > UINT32_MAX - 14u || pixel_offset < 14u + dib_size ||
        pixel_offset < sizeof(header))
        goto cleanup;

    int32_t width = (int32_t)image_read_le32(header + 18);
    int32_t raw_height = (int32_t)image_read_le32(header + 22);
    uint16_t planes = image_read_le16(header + 26);
    uint16_t bpp = image_read_le16(header + 28);
    uint32_t compression = image_read_le32(header + 30);
    if (planes != 1 || compression != 0 || (bpp != 24 && bpp != 32))
        goto cleanup;
    if (width <= 0 || raw_height == 0 || raw_height == INT32_MIN)
        goto cleanup;

    bool bottom_up = raw_height > 0;
    int32_t height = raw_height > 0 ? raw_height : -raw_height;
    if (width > 32768 || height > 32768)
        goto cleanup;

    size_t row_payload = (size_t)width * (size_t)(bpp / 8);
    size_t row_size = (bpp == 24) ? ((row_payload + 3u) & ~(size_t)3u) : row_payload;
    if (height > 0 && (size_t)height > SIZE_MAX / row_size)
        goto cleanup;
    if ((size_t)width > SIZE_MAX / (size_t)height || (size_t)width * (size_t)height > SIZE_MAX / 4u)
        goto cleanup;

    row = malloc(row_size);
    rgba = malloc((size_t)width * (size_t)height * 4u);
    if (!row || !rgba)
        goto cleanup;

    if (fseek(f, (long)pixel_offset, SEEK_SET) != 0)
        goto cleanup;

    for (int32_t src_y = 0; src_y < height; src_y++) {
        if (fread(row, 1, row_size, f) != row_size)
            goto cleanup;
        int32_t dst_y = bottom_up ? (height - 1 - src_y) : src_y;
        uint8_t *dst = rgba + ((size_t)dst_y * (size_t)width * 4u);
        for (int32_t x = 0; x < width; x++) {
            const uint8_t *src = row + (size_t)x * (size_t)(bpp / 8);
            dst[(size_t)x * 4u + 0u] = src[2];
            dst[(size_t)x * 4u + 1u] = src[1];
            dst[(size_t)x * 4u + 2u] = src[0];
            dst[(size_t)x * 4u + 3u] = bpp == 32 ? src[3] : 255u;
        }
    }

    ok = vg_image_try_set_pixels(image, rgba, width, height);

cleanup:
    free(row);
    free(rgba);
    fclose(f);
    return ok;
}

#if defined(__APPLE__)
/// @brief Decode the first frame of an ImageIO-supported file on macOS.
/// @details ImageIO renders into a premultiplied RGBA bitmap. The function
///          converts it to the widget's straight-alpha representation before
///          committing the result.
/// @param image Image widget that receives a copy of the decoded pixels.
/// @param path Null-terminated POSIX path encoded as UTF-8.
/// @return `true` when ImageIO decoded and installed the image; `false` when
///         validation, decoding, allocation, or conversion failed.
static bool image_load_platform(vg_image_t *image, const char *path) {
    if (!image || !path)
        return false;

    bool ok = false;
    CFStringRef path_ref = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    if (!path_ref)
        return false;

    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path_ref, kCFURLPOSIXPathStyle, false);
    CFRelease(path_ref);
    if (!url)
        return false;

    CGImageSourceRef source = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    if (!source)
        return false;

    CGImageRef cg_image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    CFRelease(source);
    if (!cg_image)
        return false;

    size_t width = CGImageGetWidth(cg_image);
    size_t height = CGImageGetHeight(cg_image);
    if (width == 0 || height == 0 || width > (size_t)INT_MAX || height > (size_t)INT_MAX ||
        width > SIZE_MAX / height || width * height > SIZE_MAX / 4u) {
        CGImageRelease(cg_image);
        return false;
    }

    size_t stride = width * 4u;
    uint8_t *pixels = calloc(height, stride);
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = NULL;
    if (pixels && color_space) {
        ctx = CGBitmapContextCreate(pixels,
                                    width,
                                    height,
                                    8,
                                    stride,
                                    color_space,
                                    kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    }

    if (ctx) {
        CGContextDrawImage(ctx, CGRectMake(0.0, 0.0, (CGFloat)width, (CGFloat)height), cg_image);
        image_unpremultiply_rgba(pixels, width, height);
        ok = vg_image_try_set_pixels(image, pixels, (int)width, (int)height);
    }

    if (ctx)
        CGContextRelease(ctx);
    if (color_space)
        CGColorSpaceRelease(color_space);
    free(pixels);
    CGImageRelease(cg_image);
    return ok;
}
#endif

/// @brief Source-over composite one straight-RGBA pixel onto another.
/// @details @p alpha is the already combined source and widget opacity. A fully
///          opaque source replaces the destination; partial alpha blends color
///          channels and updates destination coverage with integer rounding.
/// @param dst Mutable four-byte straight-RGBA destination pixel.
/// @param src Readable four-byte straight-RGBA source pixel.
/// @param alpha Effective source alpha in the range 0 through 255.
static void image_blend_pixel(uint8_t *dst, const uint8_t *src, uint8_t alpha) {
    if (alpha == 0)
        return;

    if (alpha == 255) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
        return;
    }

    const uint32_t inv = (uint32_t)(255 - alpha);
    dst[0] = (uint8_t)(((uint32_t)src[0] * alpha + (uint32_t)dst[0] * inv + 127u) / 255u);
    dst[1] = (uint8_t)(((uint32_t)src[1] * alpha + (uint32_t)dst[1] * inv + 127u) / 255u);
    dst[2] = (uint8_t)(((uint32_t)src[2] * alpha + (uint32_t)dst[2] * inv + 127u) / 255u);
    dst[3] = (uint8_t)(alpha + (((uint32_t)dst[3] * inv + 127u) / 255u));
}

/// @brief Release all pixel storage owned by an image widget.
/// @details Serves as the widget-vtable destruction hook. Source and scaled
///          buffers are released and their associated dimensions and capacities
///          are reset.
/// @param widget Image widget base being destroyed.
static void image_destroy(vg_widget_t *widget) {
    vg_image_t *image = (vg_image_t *)widget;
    free(image->pixels);
    free(image->scaled_pixels);
    image->pixels = NULL;
    image->scaled_pixels = NULL;
    image->pixel_capacity = 0;
    image->scaled_capacity = 0;
    image->img_width = 0;
    image->img_height = 0;
}

/// @brief Measure an image using preferred, intrinsic, then available dimensions.
/// @details Each axis independently selects its positive preferred constraint,
///          the source image's natural extent, or the corresponding available
///          extent, then applies the widget's minimum and maximum constraints.
/// @param widget Image widget base to measure.
/// @param available_width Width offered by the parent layout.
/// @param available_height Height offered by the parent layout.
static void image_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_image_t *image = (vg_image_t *)widget;

    float width = widget->constraints.preferred_width;
    float height = widget->constraints.preferred_height;

    if (width <= 0.0f) {
        width = image->img_width > 0 ? (float)image->img_width : available_width;
    }
    if (height <= 0.0f) {
        height = image->img_height > 0 ? (float)image->img_height : available_height;
    }

    widget->measured_width = width;
    widget->measured_height = height;
    vg_widget_apply_constraints(widget);
}

/// @brief Paint the background and source image into the canvas framebuffer.
/// @details Resolves the selected scale mode, intersects the raster with widget,
///          framebuffer, and active clip bounds, then chooses an opaque row-copy,
///          cached resize, or sampled alpha-blend path. Non-integral target
///          geometry falls back to deterministic per-pixel sampling.
/// @param widget Arranged image widget base to render.
/// @param canvas Backend window handle accepted by the ZannaGFX framebuffer API.
static void image_paint_content(vg_widget_t *widget, void *canvas) {
    vg_image_t *image = (vg_image_t *)widget;
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer((vgfx_window_t)canvas, &fb))
        return;

    const int dx = (int)widget->x;
    const int dy = (int)widget->y;
    const int dw = (int)widget->width;
    const int dh = (int)widget->height;
    if (dw <= 0 || dh <= 0)
        return;

    if ((image->bg_color >> 24) != 0) {
        vgfx_fill_rect((vgfx_window_t)canvas, dx, dy, dw, dh, image->bg_color);
    }

    if (!image->pixels || image->img_width <= 0 || image->img_height <= 0 || image->opacity <= 0.0f)
        return;

    const int sw = image->img_width;
    const int sh = image->img_height;
    const uint8_t opacity = (uint8_t)(image->opacity * 255.0f + 0.5f);

    float draw_x = (float)dx;
    float draw_y = (float)dy;
    float draw_w = (float)dw;
    float draw_h = (float)dh;

    if (image->scale_mode == VG_IMAGE_SCALE_NONE) {
        draw_w = (float)sw;
        draw_h = (float)sh;
    } else if (image->scale_mode == VG_IMAGE_SCALE_FIT ||
               image->scale_mode == VG_IMAGE_SCALE_FILL) {
        const float scale_x = (float)dw / (float)sw;
        const float scale_y = (float)dh / (float)sh;
        const float scale = (image->scale_mode == VG_IMAGE_SCALE_FILL)
                                ? (scale_x > scale_y ? scale_x : scale_y)
                                : (scale_x < scale_y ? scale_x : scale_y);
        draw_w = (float)sw * scale;
        draw_h = (float)sh * scale;
        draw_x = (float)dx + ((float)dw - draw_w) * 0.5f;
        draw_y = (float)dy + ((float)dh - draw_h) * 0.5f;
    }

    int start_x = clampi((int)draw_x, dx, dx + dw);
    int start_y = clampi((int)draw_y, dy, dy + dh);
    int end_x = clampi((int)(draw_x + draw_w), dx, dx + dw);
    int end_y = clampi((int)(draw_y + draw_h), dy, dy + dh);
    int clip_x = 0;
    int clip_y = 0;
    int clip_w = fb.width;
    int clip_h = fb.height;
    (void)vgfx_get_clip((vgfx_window_t)canvas, &clip_x, &clip_y, &clip_w, &clip_h);

    if (start_x < 0)
        start_x = 0;
    if (start_y < 0)
        start_y = 0;
    if (end_x > fb.width)
        end_x = fb.width;
    if (end_y > fb.height)
        end_y = fb.height;
    if (start_x < clip_x)
        start_x = clip_x;
    if (start_y < clip_y)
        start_y = clip_y;
    if (end_x > clip_x + clip_w)
        end_x = clip_x + clip_w;
    if (end_y > clip_y + clip_h)
        end_y = clip_y + clip_h;

    if (start_x >= end_x || start_y >= end_y)
        return;

    const bool integer_rect = draw_x == floorf(draw_x) && draw_y == floorf(draw_y) &&
                              draw_w == floorf(draw_w) && draw_h == floorf(draw_h) &&
                              draw_w > 0.0f && draw_h > 0.0f && draw_w <= (float)INT_MAX &&
                              draw_h <= (float)INT_MAX;
    const int raster_width = integer_rect ? (int)draw_w : 0;
    const int raster_height = integer_rect ? (int)draw_h : 0;
    const int raster_x = integer_rect ? (int)draw_x : 0;
    const int raster_y = integer_rect ? (int)draw_y : 0;

    /* The common unscaled opaque path is a clipped row copy with no per-pixel arithmetic. */
    if (integer_rect && raster_width == sw && raster_height == sh && image->pixels_opaque &&
        opacity == 255u) {
        const int source_x = start_x - raster_x;
        for (int fb_y = start_y; fb_y < end_y; ++fb_y) {
            const int source_y = fb_y - raster_y;
            memcpy(fb.pixels + (size_t)fb_y * (size_t)fb.stride + (size_t)start_x * 4u,
                   image->pixels + ((size_t)source_y * (size_t)sw + (size_t)source_x) * 4u,
                   (size_t)(end_x - start_x) * 4u);
        }
        return;
    }

    const uint8_t *scaled = NULL;
    if (integer_rect && (raster_width != sw || raster_height != sh))
        scaled = image_scaled_cache(image, raster_width, raster_height);

    if (scaled && image->pixels_opaque && opacity == 255u) {
        const int source_x = start_x - raster_x;
        for (int fb_y = start_y; fb_y < end_y; ++fb_y) {
            const int source_y = fb_y - raster_y;
            memcpy(fb.pixels + (size_t)fb_y * (size_t)fb.stride + (size_t)start_x * 4u,
                   scaled + ((size_t)source_y * (size_t)raster_width + (size_t)source_x) * 4u,
                   (size_t)(end_x - start_x) * 4u);
        }
        return;
    }

    for (int fb_y = start_y; fb_y < end_y; ++fb_y) {
        for (int fb_x = start_x; fb_x < end_x; ++fb_x) {
            uint8_t sampled[4];
            const uint8_t *src = NULL;
            if (integer_rect) {
                const int local_x = fb_x - raster_x;
                const int local_y = fb_y - raster_y;
                if (local_x < 0 || local_x >= raster_width || local_y < 0 ||
                    local_y >= raster_height) {
                    continue;
                }
                if (scaled) {
                    src = scaled + ((size_t)local_y * (size_t)raster_width + (size_t)local_x) * 4u;
                } else if (raster_width == sw && raster_height == sh) {
                    src = image->pixels + ((size_t)local_y * (size_t)sw + (size_t)local_x) * 4u;
                } else {
                    image_sample_resized(
                        image, local_x, local_y, raster_width, raster_height, sampled);
                    src = sampled;
                }
            } else {
                const int fallback_width = clampi((int)ceilf(draw_w), 1, INT_MAX);
                const int fallback_height = clampi((int)ceilf(draw_h), 1, INT_MAX);
                const int local_x = clampi((int)((float)fb_x - draw_x), 0, fallback_width - 1);
                const int local_y = clampi((int)((float)fb_y - draw_y), 0, fallback_height - 1);
                image_sample_resized(
                    image, local_x, local_y, fallback_width, fallback_height, sampled);
                src = sampled;
            }

            uint8_t alpha = src[3];
            if (alpha == 0u)
                continue;
            alpha = (uint8_t)(((uint32_t)alpha * opacity + 127u) / 255u);
            if (alpha == 0u)
                continue;
            uint8_t *dst = fb.pixels + (size_t)fb_y * (size_t)fb.stride + (size_t)fb_x * 4u;
            image_blend_pixel(dst, src, alpha);
        }
    }
}

/// @brief Paint the image content, then a keyboard-focus ring when owned.
/// @details The ring draws only for focusable-flagged images so presentation
///          images never gain chrome. It uses the shared theme focus colour,
///          keeping shortcut ownership visible exactly like other focusable
///          widgets.
/// @param widget Arranged image widget to paint.
/// @param canvas Backend window handle used for image content and focus-ring drawing.
static void image_paint(vg_widget_t *widget, void *canvas) {
    image_paint_content(widget, canvas);

    vg_image_t *image = (vg_image_t *)widget;
    if (!image->focusable || (widget->state & VG_STATE_FOCUSED) == 0)
        return;
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer((vgfx_window_t)canvas, &fb))
        return;
    const vg_theme_t *theme = vg_theme_get_current();
    if (!theme)
        return;

    const int dx = (int)widget->x;
    const int dy = (int)widget->y;
    const int dw = (int)widget->width;
    const int dh = (int)widget->height;
    if (dw <= 1 || dh <= 1)
        return;
    int thickness = (int)(theme->focus.glow_width + 0.5f);
    if (thickness < 1)
        thickness = 1;
    if (thickness * 2 > dw || thickness * 2 > dh)
        thickness = 1;
    const uint32_t ring = 0xFF000000u | (theme->focus.glow_color & 0x00FFFFFFu);
    vgfx_fill_rect((vgfx_window_t)canvas, dx, dy, dw, thickness, ring);
    vgfx_fill_rect((vgfx_window_t)canvas, dx, dy + dh - thickness, dw, thickness, ring);
    vgfx_fill_rect((vgfx_window_t)canvas, dx, dy + thickness, thickness, dh - thickness * 2, ring);
    vgfx_fill_rect((vgfx_window_t)canvas,
                   dx + dw - thickness,
                   dy + thickness,
                   thickness,
                   dh - thickness * 2,
                   ring);
}

/// @brief Create an image widget with no initial pixel data.
///
/// @details The widget is created with VG_IMAGE_SCALE_FIT and full opacity.
///          Pixel data must be supplied via vg_image_set_pixels or
///          vg_image_load_file before the widget displays anything.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @return Newly allocated vg_image_t, or NULL on allocation failure.
vg_image_t *vg_image_create(vg_widget_t *parent) {
    vg_image_t *image = calloc(1, sizeof(vg_image_t));
    if (!image)
        return NULL;

    vg_widget_init(&image->base, VG_WIDGET_IMAGE, &g_image_vtable);

    image->scale_mode = VG_IMAGE_SCALE_FIT;
    image->filter = VG_IMAGE_FILTER_NEAREST;
    image->scaled_filter = VG_IMAGE_FILTER_NEAREST;
    image->pixels_opaque = true;
    image->content_revision = 1u;
    image->opacity = 1.0f;
    image->bg_color = 0x00000000;

    if (parent) {
        vg_widget_add_child(parent, &image->base);
    }

    return image;
}

/// @brief Replace the image's pixel data with a copy of the supplied buffer.
///
/// @details Passing NULL pixels or non-positive dimensions clears the image and
///          marks it for re-layout and repaint. Valid data forwards to the atomic
///          @ref vg_image_try_set_pixels path and preserves the previous image if
///          allocation fails.
///
/// @param image  The image widget to update.
/// @param pixels Pointer to width×height RGBA pixels (4 bytes per pixel,
///               straight alpha, top-left origin).  May be NULL to clear.
/// @param width  Source image width in pixels; must be > 0 when pixels != NULL.
/// @param height Source image height in pixels; must be > 0 when pixels != NULL.
void vg_image_set_pixels(vg_image_t *image, const uint8_t *pixels, int width, int height) {
    if (!image)
        return;

    if (!pixels || width <= 0 || height <= 0) {
        vg_image_clear(image);
        return;
    }

    (void)vg_image_try_set_pixels(image, pixels, width, height);
}

/// @brief Atomically replace the image's pixels, reusing sufficient storage.
/// @details All validation and any required allocation precede mutation. An identical update is a
///          successful no-op and does not advance revisions or dirty retained state.
/// @param image Image widget to mutate.
/// @param pixels Complete straight-alpha RGBA source buffer.
/// @param width Positive source width.
/// @param height Positive source height.
/// @return true after a complete copy; false with the old state intact on failure.
bool vg_image_try_set_pixels(vg_image_t *image, const uint8_t *pixels, int width, int height) {
    size_t size = 0;
    if (!image || !pixels || !image_rgba_size(width, height, &size))
        return false;

    if (image->pixels && image->img_width == width && image->img_height == height &&
        memcmp(image->pixels, pixels, size) == 0) {
        return true;
    }

    uint8_t *target = image->pixels;
    size_t target_capacity = image->pixel_capacity;
    if (!target || size > target_capacity) {
        target = (uint8_t *)malloc(size);
        if (!target)
            return false;
        memcpy(target, pixels, size);
    } else {
        memmove(target, pixels, size);
    }

    const bool layout_changed = image->img_width != width || image->img_height != height;
    if (target != image->pixels) {
        free(image->pixels);
        image->pixels = target;
        image->pixel_capacity = size;
    }
    image->img_width = width;
    image->img_height = height;
    image->pixels_opaque = image_pixels_are_opaque(image->pixels, size);
    image_note_content_change(image, layout_changed);
    return true;
}

/// @brief Determine whether two pointer-sized byte ranges overlap.
/// @param first Start of the first range.
/// @param first_size Size of the first range in bytes.
/// @param second Start of the second range.
/// @param second_size Size of the second range in bytes.
/// @return true when the ranges share at least one byte.
static bool image_ranges_overlap(const void *first,
                                 size_t first_size,
                                 const void *second,
                                 size_t second_size) {
    if (!first || !second || first_size == 0u || second_size == 0u)
        return false;
    const uintptr_t first_start = (uintptr_t)first;
    const uintptr_t second_start = (uintptr_t)second;
    const uintptr_t first_end =
        first_size > UINTPTR_MAX - first_start ? UINTPTR_MAX : first_start + (uintptr_t)first_size;
    const uintptr_t second_end = second_size > UINTPTR_MAX - second_start
                                     ? UINTPTR_MAX
                                     : second_start + (uintptr_t)second_size;
    return first_start < second_end && second_start < first_end;
}

/// @brief Copy a validated source rectangle into an existing image atomically.
/// @details A temporary buffer is used only when caller storage overlaps the owned destination.
///          Invalid rectangles and overlap-buffer allocation failure leave destination bytes and
///          revisions unchanged.
/// @param image Image widget whose existing pixel buffer is updated.
/// @param pixels Complete straight-alpha RGBA source image.
/// @param source_width Width of the source image in pixels.
/// @param source_height Height of the source image in pixels.
/// @param source_x Left edge of the source rectangle.
/// @param source_y Top edge of the source rectangle.
/// @param width Width of the copied rectangle.
/// @param height Height of the copied rectangle.
/// @param dest_x Left edge of the destination rectangle.
/// @param dest_y Top edge of the destination rectangle.
/// @return `true` after a valid complete update, including an identical no-op;
///         `false` when validation or temporary allocation failed.
bool vg_image_update_region(vg_image_t *image,
                            const uint8_t *pixels,
                            int source_width,
                            int source_height,
                            int source_x,
                            int source_y,
                            int width,
                            int height,
                            int dest_x,
                            int dest_y) {
    size_t source_size = 0;
    size_t destination_size = 0;
    size_t region_size = 0;
    if (!image || !image->pixels || !pixels ||
        !image_rgba_size(source_width, source_height, &source_size) ||
        !image_rgba_size(image->img_width, image->img_height, &destination_size) ||
        !image_rgba_size(width, height, &region_size) || source_x < 0 || source_y < 0 ||
        dest_x < 0 || dest_y < 0 || source_x > source_width - width ||
        source_y > source_height - height || dest_x > image->img_width - width ||
        dest_y > image->img_height - height) {
        return false;
    }

    const uint8_t *copy_source = pixels;
    int copy_stride = source_width;
    uint8_t *temporary = NULL;
    if (image_ranges_overlap(pixels, source_size, image->pixels, destination_size)) {
        temporary = (uint8_t *)malloc(region_size);
        if (!temporary)
            return false;
        for (int row = 0; row < height; ++row) {
            memcpy(temporary + (size_t)row * (size_t)width * 4u,
                   pixels +
                       ((size_t)(source_y + row) * (size_t)source_width + (size_t)source_x) * 4u,
                   (size_t)width * 4u);
        }
        copy_source = temporary;
        copy_stride = width;
        source_x = 0;
        source_y = 0;
    }

    bool identical = true;
    for (int row = 0; row < height; ++row) {
        const uint8_t *source_row =
            copy_source + ((size_t)(source_y + row) * (size_t)copy_stride + (size_t)source_x) * 4u;
        const uint8_t *dest_row =
            image->pixels +
            ((size_t)(dest_y + row) * (size_t)image->img_width + (size_t)dest_x) * 4u;
        if (memcmp(source_row, dest_row, (size_t)width * 4u) != 0) {
            identical = false;
            break;
        }
    }
    if (identical) {
        free(temporary);
        return true;
    }

    for (int row = 0; row < height; ++row) {
        const uint8_t *source_row =
            copy_source + ((size_t)(source_y + row) * (size_t)copy_stride + (size_t)source_x) * 4u;
        uint8_t *dest_row =
            image->pixels +
            ((size_t)(dest_y + row) * (size_t)image->img_width + (size_t)dest_x) * 4u;
        memcpy(dest_row, source_row, (size_t)width * 4u);
    }
    free(temporary);
    image->pixels_opaque = image_pixels_are_opaque(image->pixels, destination_size);
    image_note_content_change(image, false);
    return true;
}

/// @brief Load an image from a file path into the widget's pixel buffer.
///
/// @details On macOS, any format supported by ImageIO (PNG, JPEG, TIFF, …) is
///          tried first; all platforms fall back to the built-in BMP decoder.
///
/// @param image The image widget to populate.
/// @param path  Null-terminated path to the image file.
/// @return true on success; false if the file cannot be opened or decoded.
bool vg_image_load_file(vg_image_t *image, const char *path) {
    if (!image || !path)
        return false;

#if defined(__APPLE__)
    if (image_load_platform(image, path))
        return true;
#endif
    return image_load_bmp(image, path);
}

/// @brief Free the image's pixel buffer and mark it for re-layout and repaint.
///
/// @param image The image widget to clear.
void vg_image_clear(vg_image_t *image) {
    if (!image)
        return;
    if (!image->pixels && image->img_width == 0 && image->img_height == 0)
        return;
    free(image->pixels);
    image->pixels = NULL;
    image->pixel_capacity = 0;
    image->img_width = 0;
    image->img_height = 0;
    image->pixels_opaque = true;
    image_note_content_change(image, true);
}

/// @brief Set how pixel data is scaled to fill the widget bounds.
///
/// @param image The image widget to configure.
/// @param mode  VG_IMAGE_SCALE_NONE  — draw at natural size from widget origin.
///              VG_IMAGE_SCALE_FIT   — scale uniformly to fit, letterboxing if needed.
///              VG_IMAGE_SCALE_FILL  — scale uniformly to fill, cropping excess.
///              VG_IMAGE_SCALE_STRETCH — stretch independently on each axis.
void vg_image_set_scale_mode(vg_image_t *image, vg_image_scale_t mode) {
    if (!image)
        return;
    if (mode < VG_IMAGE_SCALE_NONE || mode > VG_IMAGE_SCALE_STRETCH)
        mode = VG_IMAGE_SCALE_FIT;
    if (image->scale_mode == mode)
        return;
    image->scale_mode = mode;
    vg_widget_note_revision(&image->base);
    vg_widget_invalidate(&image->base);
}

/// @brief Select nearest-neighbor or bilinear resize filtering.
/// @details Unsupported values normalize to nearest-neighbor. A changed filter
///          invalidates cached resized pixels and schedules repainting.
/// @param image Image widget to configure; `NULL` is ignored.
/// @param filter Requested filter enumerator.
void vg_image_set_filter(vg_image_t *image, vg_image_filter_t filter) {
    if (!image)
        return;
    if (filter != VG_IMAGE_FILTER_BILINEAR)
        filter = VG_IMAGE_FILTER_NEAREST;
    if (image->filter == filter)
        return;
    image->filter = filter;
    image_invalidate_scaled_cache(image);
    vg_widget_note_revision(&image->base);
    vg_widget_invalidate(&image->base);
}

/// @brief Query the active image resize filter.
/// @param image Image widget to inspect, or `NULL`.
/// @return The widget's filter, or VG_IMAGE_FILTER_NEAREST for `NULL`.
vg_image_filter_t vg_image_get_filter(const vg_image_t *image) {
    return image ? image->filter : VG_IMAGE_FILTER_NEAREST;
}

/// @brief Set the overall opacity applied when blending the image onto the canvas.
///
/// @param image   The image widget to configure.
/// @param opacity Opacity in [0.0, 1.0]; clamped and non-finite values default
///                to 1.0.  0.0 makes the image fully transparent (still laid out);
///                1.0 renders it fully opaque.
void vg_image_set_opacity(vg_image_t *image, float opacity) {
    if (!image)
        return;
    if (!isfinite(opacity))
        opacity = 1.0f;
    if (opacity < 0)
        opacity = 0;
    if (opacity > 1)
        opacity = 1;
    if (image->opacity == opacity)
        return;
    image->opacity = opacity;
    vg_widget_note_revision(&image->base);
    vg_widget_invalidate(&image->base);
}

/// @brief Opt an image into keyboard focus (click-to-focus plus a focus ring).
/// @param image Image widget; NULL is ignored.
/// @param focusable true to accept focus; false restores presentation-only.
void vg_image_set_focusable(vg_image_t *image, bool focusable) {
    if (!image || image->focusable == focusable)
        return;
    image->focusable = focusable;
    vg_widget_invalidate(&image->base);
}
