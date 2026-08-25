//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_pixels_internal.h
// Purpose: Shared internal definitions for the Pixels (software image buffer)
//   runtime implementation. Exposes the rt_pixels_impl struct and common
//   helpers so that the core, I/O, transform, and draw modules can all access
//   pixel data directly.
//
// Key invariants:
//   - This header is INTERNAL to the runtime — never include from public APIs.
//   - Runtime graphics modules that need direct pixel access include this header.
//   - The public API header (rt_pixels.h) remains the sole interface for callers.
//   - Pixel format is 32-bit RGBA: 0xRRGGBBAA in row-major order.
//
// Ownership/Lifetime:
//   - The rt_pixels_impl struct is GC-managed via rt_obj_new_i64.
//   - Pixel data is embedded in the GC allocation (not separately malloc'd).
//
// Links: src/runtime/graphics/2d/rt_pixels.h (public API),
//        src/runtime/graphics/2d/rt_pixels.c (core operations),
//        src/runtime/graphics/2d/rt_pixels_io.c (BMP/PNG/JPEG I/O),
//        src/runtime/graphics/2d/rt_pixels_transform.c (geometry and effects),
//        src/runtime/graphics/2d/rt_pixels_draw.c (drawing primitives)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the private Pixels payload and shared inline operations.
///
/// This header centralizes opaque-handle validation, runtime color
/// normalization, saturating coordinate arithmetic, rectangle and copy
/// clipping, mutation-generation invalidation, alpha classification, and
/// source-over composition. It is an implementation seam for the split
/// Pixels translation units and is not a public C ABI header.
#pragma once

#include "rt_object.h"
#include "rt_pixels.h"

#include <limits.h>
#include <stdint.h>

/// Marks the runtime's tagged `Color.RGBA` representation.
/// @details The low 32 bits of a tagged value use `0xAARRGGBB`; the marker
///          distinguishes it from Canvas RGB and raw Pixels RGBA integers.
#define RT_PIXELS_COLOR_EXPLICIT_ALPHA_FLAG (INT64_C(1) << 56)

/// @brief Cached alpha-content classes used by deferred material routing.
/// @details Opaque contains only alpha 255, binary contains alpha 0/255 with at least one zero,
///   and fractional contains at least one alpha value strictly between 0 and 255. Values are
///   ordered from cheapest/most depth-friendly routing to full blending.
typedef enum rt_pixels_alpha_classification {
    RT_PIXELS_ALPHA_OPAQUE = 0,
    RT_PIXELS_ALPHA_BINARY = 1,
    RT_PIXELS_ALPHA_FRACTIONAL = 2
} rt_pixels_alpha_classification;

/// @brief GC-managed payload behind every opaque Pixels handle.
/// @details Nonempty instances store the row-major pixel words immediately
///          after this header in the same runtime allocation. Mutation
///          generation and alpha-cache state are intentionally colocated so
///          render caches can key and invalidate without rescanning eagerly.
typedef struct rt_pixels_impl {
    int64_t width;                  ///< Width in pixels.
    int64_t height;                 ///< Height in pixels.
    uint32_t *data;                 ///< Pixel storage (RGBA, row-major).
    uint64_t generation;            ///< Monotonic content version for GPU caches.
    uint64_t cache_identity;        ///< Stable cache key to survive allocator address reuse.
    uint64_t alpha_scan_generation; ///< Pixels generation used for cached alpha classification.
    uint64_t alpha_classification_scan_count; ///< Lifetime cache-miss classifications (diagnostic).
    int8_t alpha_scan_valid;          ///< True when the cached class matches alpha_scan_generation.
    int8_t alpha_scan_classification; ///< Cached `rt_pixels_alpha_classification` value.
} rt_pixels_impl;

/// @brief Report an invalid opaque Pixels handle without exposing runtime
///        definition symbols in this internal header.
/// @param op Operation-specific diagnostic text, or null when the caller has
///        no override.
/// @param fallback Fallback trap text used when @p op is null.
void zanna_pixels_trap_invalid_handle(const char *op, const char *fallback);

/// @brief Validate and cast an opaque handle to a Pixels implementation.
/// @details Normal runtime builds require the expected class identifier and
///          minimum payload size. Invalid input traps through
///          `zanna_pixels_trap_invalid_handle()` and returns null. Specialized
///          internal builds defining `RT_PIXELS_INTERNAL_ASSUME_STRUCT_HANDLE`
///          use a direct cast and skip runtime-object validation.
/// @param pixels Opaque candidate handle.
/// @param op Operation-specific diagnostic text for validation failures.
/// @return The validated implementation pointer, or null after a validation
///         trap.
static inline rt_pixels_impl *rt_pixels_checked_impl(void *pixels, const char *op) {
#ifdef RT_PIXELS_INTERNAL_ASSUME_STRUCT_HANDLE
    (void)op;
    return (rt_pixels_impl *)pixels;
#else
    if (!pixels) {
        zanna_pixels_trap_invalid_handle(op, "Pixels: null pixels");
        return NULL;
    }
    if (!rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl))) {
        zanna_pixels_trap_invalid_handle(op, "Pixels: invalid pixels");
        return NULL;
    }
    rt_pixels_impl *impl = (rt_pixels_impl *)pixels;
    if (impl->width < 0 || impl->height < 0 ||
        (impl->width != 0 && impl->height > INT64_MAX / impl->width)) {
        zanna_pixels_trap_invalid_handle(op, "Pixels: invalid layout");
        return NULL;
    }
    int64_t count = impl->width * impl->height;
    if (count > (INT64_MAX - (int64_t)sizeof(*impl)) / (int64_t)sizeof(uint32_t) ||
        (uint64_t)count >
            ((uint64_t)SIZE_MAX - (uint64_t)sizeof(*impl)) / (uint64_t)sizeof(uint32_t)) {
        zanna_pixels_trap_invalid_handle(op, "Pixels: invalid layout");
        return NULL;
    }
    size_t required = sizeof(*impl) + (size_t)count * sizeof(uint32_t);
    uint32_t *expected_data = count > 0 ? (uint32_t *)((uint8_t *)impl + sizeof(*impl)) : NULL;
    if (!rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, required) || impl->data != expected_data ||
        impl->cache_identity == 0 || (impl->alpha_scan_valid != 0 && impl->alpha_scan_valid != 1) ||
        (impl->alpha_scan_valid &&
         (impl->alpha_scan_generation != impl->generation ||
          impl->alpha_scan_classification < RT_PIXELS_ALPHA_OPAQUE ||
          impl->alpha_scan_classification > RT_PIXELS_ALPHA_FRACTIONAL))) {
        zanna_pixels_trap_invalid_handle(op, "Pixels: invalid layout");
        return NULL;
    }
    return impl;
#endif
}

/// @brief Validate an optional Pixels handle without trapping.
/// @details The normal path accepts only an instance of the Pixels runtime
///          class with a complete implementation payload. The struct-handle
///          specialization performs only the direct cast.
/// @param pixels Optional opaque candidate handle.
/// @return The validated implementation pointer, or null for absent or
///         invalid input.
static inline rt_pixels_impl *rt_pixels_checked_impl_or_null(void *pixels) {
#ifdef RT_PIXELS_INTERNAL_ASSUME_STRUCT_HANDLE
    return (rt_pixels_impl *)pixels;
#else
    if (!pixels || !rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl)))
        return NULL;
    rt_pixels_impl *impl = (rt_pixels_impl *)pixels;
    if (impl->width < 0 || impl->height < 0 ||
        (impl->width != 0 && impl->height > INT64_MAX / impl->width))
        return NULL;
    int64_t count = impl->width * impl->height;
    if (count > (INT64_MAX - (int64_t)sizeof(*impl)) / (int64_t)sizeof(uint32_t) ||
        (uint64_t)count >
            ((uint64_t)SIZE_MAX - (uint64_t)sizeof(*impl)) / (uint64_t)sizeof(uint32_t))
        return NULL;
    size_t required = sizeof(*impl) + (size_t)count * sizeof(uint32_t);
    uint32_t *expected_data = count > 0 ? (uint32_t *)((uint8_t *)impl + sizeof(*impl)) : NULL;
    if (!rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, required) || impl->data != expected_data ||
        impl->cache_identity == 0 || (impl->alpha_scan_valid != 0 && impl->alpha_scan_valid != 1) ||
        (impl->alpha_scan_valid && (impl->alpha_scan_generation != impl->generation ||
                                    impl->alpha_scan_classification < RT_PIXELS_ALPHA_OPAQUE ||
                                    impl->alpha_scan_classification > RT_PIXELS_ALPHA_FRACTIONAL)))
        return NULL;
    return impl;
#endif
}

/// @brief Convert 0x00RRGGBB canvas color to 0xRRGGBBFF (fully-opaque RGBA).
/// @param color Integer whose low 24 bits contain Canvas-style RGB channels.
/// @return Canonical raw Pixels `0xRRGGBBFF`.
static inline uint32_t rgb_to_rgba(int64_t color) {
    uint32_t rgb = (uint32_t)((uint64_t)color & 0x00FFFFFFu);
    return (rgb << 8) | 0xFFu;
}

/// @brief Preserve a caller-supplied raw 0xRRGGBBAA value.
/// @param rgba Integer whose low 32 bits contain canonical raw Pixels RGBA.
/// @return The low 32 bits unchanged.
static inline uint32_t rt_pixels_raw_rgba(int64_t rgba) {
    return (uint32_t)((uint64_t)rgba & 0xFFFFFFFFu);
}

/// @brief Normalize any public drawing-color form to raw `0xRRGGBBAA`.
/// @details Explicitly tagged `Color.RGBA` stores `0xAARRGGBB` in its low
///          word and is reordered. Untagged values at most `0x00FFFFFF` are
///          opaque Canvas RGB; larger untagged values are already raw Pixels
///          RGBA and retain their low 32 bits.
/// @param color Canvas RGB, raw Pixels RGBA, or tagged runtime `Color.RGBA`.
/// @return Canonical raw Pixels `0xRRGGBBAA`.
static inline uint32_t rt_pixels_color_to_rgba(int64_t color) {
    uint64_t c = (uint64_t)color;
    if ((c & (uint64_t)RT_PIXELS_COLOR_EXPLICIT_ALPHA_FLAG) != 0) {
        uint32_t argb = (uint32_t)c;
        uint32_t a = (argb >> 24) & 0xFFu;
        uint32_t r = (argb >> 16) & 0xFFu;
        uint32_t g = (argb >> 8) & 0xFFu;
        uint32_t b = argb & 0xFFu;
        return (r << 24) | (g << 16) | (b << 8) | a;
    }
    if (c <= 0x00FFFFFFu)
        return rgb_to_rgba(color);
    return rt_pixels_raw_rgba(color);
}

/// @brief Preserve raw RGBA unless the value is an explicitly tagged Color.RGBA.
/// @details Unlike `rt_pixels_color_to_rgba()`, an untagged 24-bit value is
///          treated as raw RGBA rather than opaque Canvas RGB.
/// @param color Raw Pixels RGBA or tagged runtime `Color.RGBA`.
/// @return Canonical raw Pixels `0xRRGGBBAA`.
static inline uint32_t rt_pixels_rgba_or_tagged_color_to_rgba(int64_t color) {
    uint64_t c = (uint64_t)color;
    if ((c & (uint64_t)RT_PIXELS_COLOR_EXPLICIT_ALPHA_FLAG) == 0)
        return rt_pixels_raw_rgba(color);
    return rt_pixels_color_to_rgba(color);
}

/// @brief Convert raw 0xRRGGBBAA storage into a tagged Color.RGBA-compatible value.
/// @param rgba Canonical raw Pixels `0xRRGGBBAA`.
/// @return Runtime-tagged value with `0xAARRGGBB` in the low word and
///         `RT_PIXELS_COLOR_EXPLICIT_ALPHA_FLAG` set.
static inline int64_t rt_pixels_rgba_to_color(uint32_t rgba) {
    uint32_t r = (rgba >> 24) & 0xFFu;
    uint32_t g = (rgba >> 16) & 0xFFu;
    uint32_t b = (rgba >> 8) & 0xFFu;
    uint32_t a = rgba & 0xFFu;
    uint32_t argb = (a << 24) | (r << 16) | (g << 8) | b;
    return (int64_t)argb | RT_PIXELS_COLOR_EXPLICIT_ALPHA_FLAG;
}

/// @brief Write one pixel with bounds check (no null check — caller ensures p is valid).
/// @param p Valid destination implementation with writable storage.
/// @param x Zero-based destination column.
/// @param y Zero-based destination row.
/// @param c Canonical raw `0xRRGGBBAA` value.
/// @return 1 if the stored value changed, 0 if the coordinate was outside the
///         buffer or already contained @p c.
static inline int8_t set_pixel_raw(rt_pixels_impl *p, int64_t x, int64_t y, uint32_t c) {
    if (x >= 0 && x < p->width && y >= 0 && y < p->height) {
        uint32_t *pixel = &p->data[y * p->width + x];
        if (*pixel == c)
            return 0;
        *pixel = c;
        return 1;
    }
    return 0;
}

/// @brief Bump the image content generation after an in-place mutation.
/// @details Also invalidates the generation-keyed alpha classification. A null
///          pointer is ignored. Generation zero is reserved for newly
///          constructed images, so wraparound skips zero.
/// @param p Mutated implementation, or null.
static inline void pixels_touch(rt_pixels_impl *p) {
    if (p) {
        p->generation++;
        if (p->generation == 0)
            p->generation = 1;
        p->alpha_scan_valid = 0;
    }
}

/// @brief Reuse a source image's cached alpha classification for an exact
///        alpha-preserving result.
/// @details The destination keeps its own generation and diagnostic scan
///          count. A stale or absent source cache leaves the destination cache
///          invalid instead of copying an unverified classification.
/// @param dst Newly produced destination image.
/// @param src Source image whose multiset of alpha values is preserved.
static inline void pixels_copy_alpha_classification_cache(rt_pixels_impl *dst,
                                                          const rt_pixels_impl *src) {
    if (!dst)
        return;
    if (src && src->alpha_scan_valid && src->alpha_scan_generation == src->generation) {
        dst->alpha_scan_generation = dst->generation;
        dst->alpha_scan_classification = src->alpha_scan_classification;
        dst->alpha_scan_valid = 1;
    } else {
        dst->alpha_scan_valid = 0;
    }
}

/// @brief Classify a Pixels buffer as opaque, binary-alpha, or fractional-alpha by generation.
/// @details A cache miss performs one linear scan. It remembers a zero-alpha texel while
///   continuing until a fractional texel is found; therefore the same pass supplies both the
///   alpha-bearing and blend-vs-mask decisions. `pixels_touch` invalidates the result after every
///   supported write. Corrupt dimension multiplication is classified as fractional, the safest
///   conservative route because blending cannot incorrectly depth-occlude geometry behind it.
/// @warning This internal cache is main-render-thread state. Concurrent classification/mutation of
///   one Pixels object is unsupported and could observe torn cache fields.
/// @param p Pixels payload, or NULL.
/// @return One `rt_pixels_alpha_classification` value; invalid/empty buffers are opaque.
static inline rt_pixels_alpha_classification rt_pixels_alpha_classification_cached(
    rt_pixels_impl *p) {
    uint64_t count;
    rt_pixels_alpha_classification classification = RT_PIXELS_ALPHA_OPAQUE;
    if (!p || p->width <= 0 || p->height <= 0 || !p->data)
        return RT_PIXELS_ALPHA_OPAQUE;
    if (p->alpha_scan_valid && p->alpha_scan_generation == p->generation)
        return (rt_pixels_alpha_classification)p->alpha_scan_classification;
    if (p->alpha_classification_scan_count < UINT64_MAX)
        p->alpha_classification_scan_count++;
    if ((uint64_t)p->width > UINT64_MAX / (uint64_t)p->height) {
        p->alpha_scan_generation = p->generation;
        p->alpha_scan_classification = RT_PIXELS_ALPHA_FRACTIONAL;
        p->alpha_scan_valid = 1;
        return RT_PIXELS_ALPHA_FRACTIONAL;
    }
    count = (uint64_t)p->width * (uint64_t)p->height;
    for (uint64_t i = 0; i < count; i++) {
        uint32_t alpha = p->data[i] & 0xFFu;
        if (alpha > 0u && alpha < 0xFFu) {
            classification = RT_PIXELS_ALPHA_FRACTIONAL;
            break;
        }
        if (alpha == 0u)
            classification = RT_PIXELS_ALPHA_BINARY;
    }
    p->alpha_scan_generation = p->generation;
    p->alpha_scan_classification = (int8_t)classification;
    p->alpha_scan_valid = 1;
    return classification;
}

/// @brief Return whether cached Pixels classification contains any non-opaque alpha.
/// @details Compatibility wrapper for TextureAsset3D metadata callers; it reuses the same single
///   generation-keyed classification scan as Canvas3D's blend/mask routing.
/// @param p Pixels payload, or NULL.
/// @return Non-zero for binary or fractional alpha, zero for opaque/invalid buffers.
static inline int rt_pixels_has_alpha_texels_cached(rt_pixels_impl *p) {
    return rt_pixels_alpha_classification_cached(p) != RT_PIXELS_ALPHA_OPAQUE;
}

/// @brief Add signed clipping coordinates with saturation.
/// @param a First addend.
/// @param b Second addend.
/// @return `a + b` clamped to the signed 64-bit range.
static inline int64_t rt_pixels_add_sat64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Compute a saturating absolute signed-coordinate difference.
/// @param a First coordinate.
/// @param b Second coordinate.
/// @return `abs(a - b)` clamped to `INT64_MAX`.
static inline int64_t rt_pixels_abs_diff_sat64(int64_t a, int64_t b) {
    uint64_t diff = a >= b ? (uint64_t)a - (uint64_t)b : (uint64_t)b - (uint64_t)a;
    if (diff > (uint64_t)INT64_MAX)
        return INT64_MAX;
    return (int64_t)diff;
}

/// @brief Subtract a non-negative extent with saturation.
/// @details A non-positive @p b leaves @p a unchanged, matching callers that
///          normalize optional radii or lengths before clipping.
/// @param a Starting coordinate.
/// @param b Extent to subtract.
/// @return @p a unchanged when @p b is non-positive; otherwise `a - b`
///         clamped to `INT64_MIN`.
static inline int64_t rt_pixels_sub_nonneg_sat64(int64_t a, int64_t b) {
    if (b <= 0)
        return a;
    if (a < INT64_MIN + b)
        return INT64_MIN;
    return a - b;
}

/// @brief Count the leading units clipped by a negative start coordinate.
/// @param start Requested first coordinate.
/// @param len Requested non-negative span length.
/// @return Zero for a non-negative start or non-positive length; otherwise
///         `-start` capped at @p len, with `INT64_MIN` handled without
///         negation overflow.
static inline int64_t rt_pixels_negative_skip(int64_t start, int64_t len) {
    if (start >= 0 || len <= 0)
        return 0;
    if (start == INT64_MIN)
        return len;
    int64_t skip = -start;
    return skip >= len ? len : skip;
}

/// @brief Clip paired source and destination spans to two bounded axes.
/// @details Negative source coordinates advance the destination by the same
///          amount; negative destination coordinates advance the source.
///          The surviving length is then limited by both upper bounds. On
///          failure, @p len is set to zero whenever it is available.
/// @param dst_limit Exclusive non-negative destination bound.
/// @param src_limit Exclusive non-negative source bound.
/// @param dst In/out destination start coordinate.
/// @param src In/out source start coordinate.
/// @param len In/out paired span length.
/// @return Nonzero when a positive paired span survives; zero for invalid
///         pointers, invalid limits, overflow, or no intersection.
static inline int8_t rt_pixels_clip_copy_axis(
    int64_t dst_limit, int64_t src_limit, int64_t *dst, int64_t *src, int64_t *len) {
    if (!dst || !src || !len || *len <= 0 || dst_limit <= 0 || src_limit <= 0) {
        if (len)
            *len = 0;
        return 0;
    }

    int64_t skip = rt_pixels_negative_skip(*src, *len);
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

    skip = rt_pixels_negative_skip(*dst, *len);
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

/// @brief Clip an in-place rectangle to a pixel buffer without endpoint overflow.
/// @details Negative origins reduce the corresponding extent before being
///          moved to zero. Remaining widths and heights are limited with
///          subtraction from the destination dimensions, avoiding `x + w`
///          and `y + h` overflow.
/// @param p Destination implementation with valid dimensions and storage.
/// @param x In/out rectangle left coordinate.
/// @param y In/out rectangle top coordinate.
/// @param w In/out rectangle width.
/// @param h In/out rectangle height.
/// @return Nonzero when a positive clipped rectangle remains; zero for
///         invalid input, empty dimensions, or no intersection.
static inline int8_t rt_pixels_clip_rect_to_bounds(
    rt_pixels_impl *p, int64_t *x, int64_t *y, int64_t *w, int64_t *h) {
    if (!p || !p->data || !x || !y || !w || !h || *w <= 0 || *h <= 0 || p->width <= 0 ||
        p->height <= 0)
        return 0;

    int64_t skip = rt_pixels_negative_skip(*x, *w);
    if (skip >= *w)
        return 0;
    *x = skip > 0 ? 0 : *x;
    *w -= skip;

    skip = rt_pixels_negative_skip(*y, *h);
    if (skip >= *h)
        return 0;
    *y = skip > 0 ? 0 : *y;
    *h -= skip;

    if (*x >= p->width || *y >= p->height)
        return 0;
    int64_t x_remaining = p->width - *x;
    int64_t y_remaining = p->height - *y;
    if (*w > x_remaining)
        *w = x_remaining;
    if (*h > y_remaining)
        *h = y_remaining;
    return *w > 0 && *h > 0;
}

/// @brief Compose two straight-alpha RGBA pixels using Porter-Duff source-over.
/// @details Both operands and the result use canonical `0xRRGGBBAA`.
///          Transparent and opaque source pixels take exact fast paths;
///          intermediate alpha is calculated in premultiplied channel space
///          with integer rounding and converted back to straight alpha.
/// @param dst Destination pixel beneath the source.
/// @param src Source pixel placed over the destination.
/// @return The straight-alpha source-over result as `0xRRGGBBAA`.
static inline uint32_t rt_pixels_alpha_over_rgba(uint32_t dst, uint32_t src) {
    uint32_t sr = (src >> 24) & 0xFFu;
    uint32_t sg = (src >> 16) & 0xFFu;
    uint32_t sb = (src >> 8) & 0xFFu;
    uint32_t sa = src & 0xFFu;
    uint32_t dr = (dst >> 24) & 0xFFu;
    uint32_t dg = (dst >> 16) & 0xFFu;
    uint32_t db = (dst >> 8) & 0xFFu;
    uint32_t da = dst & 0xFFu;

    if (sa == 0)
        return dst;
    if (sa == 255)
        return src;

    uint32_t inv = 255u - sa;
    uint32_t oa = sa + (da * inv + 127u) / 255u;
    if (oa == 0)
        return 0;

    uint32_t r_pm = sr * sa + (dr * da * inv + 127u) / 255u;
    uint32_t g_pm = sg * sa + (dg * da * inv + 127u) / 255u;
    uint32_t b_pm = sb * sa + (db * da * inv + 127u) / 255u;
    uint32_t r = (r_pm + oa / 2u) / oa;
    uint32_t g = (g_pm + oa / 2u) / oa;
    uint32_t b = (b_pm + oa / 2u) / oa;
    if (r > 255u)
        r = 255u;
    if (g > 255u)
        g = 255u;
    if (b > 255u)
        b = 255u;
    return (r << 24) | (g << 16) | (b << 8) | oa;
}

/// @brief Compute the floor of a signed 64-bit integer square root.
/// @details Uses Newton iteration for positive input. Non-positive values
///          return zero.
/// @param n Radicand.
/// @return `floor(sqrt(n))` for positive @p n; otherwise zero.
static inline int64_t isqrt64(int64_t n) {
    if (n <= 0)
        return 0;
    int64_t x = n;
    int64_t y = x / 2 + x % 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

/// @brief Allocate a new Pixels object with embedded pixel data.
/// @details Defined in rt_pixels.c. Creates a GC-managed Pixels object with
///          zero-filled pixel data embedded in the allocation. Zero
///          dimensions produce a valid object without a data region; negative
///          dimensions return null, while overflow and allocation failure
///          report runtime traps in the implementation.
/// @param width Non-negative image width.
/// @param height Non-negative image height.
/// @return A new implementation with generation zero and a unique cache
///         identity, or null when construction fails.
rt_pixels_impl *pixels_alloc(int64_t width, int64_t height);

/// @brief Extract, nearest-scale, optionally flip, tint, and alpha-modulate a
///        source region in one destination pass.
/// @details Internal rendering helper; rotation is intentionally separate.
#ifdef __cplusplus
extern "C"
#endif
    void *rt_pixels_transform_region_nearest(void *pixels,
                                             int64_t source_x,
                                             int64_t source_y,
                                             int64_t source_width,
                                             int64_t source_height,
                                             int64_t destination_width,
                                             int64_t destination_height,
                                             int8_t flip_x,
                                             int8_t flip_y,
                                             int64_t tint_color,
                                             int64_t alpha);
