//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_camera.c
/// @file
/// @brief Implements the reference-counted 2D camera transform, bounds,
///        culling, follow behavior, and tiled parallax backgrounds.
// Purpose: 2D camera transform for Zanna game scenes. Maintains a world-space
//   viewport defined by a position, an integer zoom percentage, and an optional
//   rotation angle. Provides coordinate conversion (world↔screen), optional
//   world-bounds clamping, viewport culling, and a dirty flag to let renderers
//   skip unnecessary redraws when the camera hasn't moved.
//
// Key invariants:
//   - All coordinates are integers (pixels). Zoom is an integer percentage:
//     100 = 1× (no zoom), 200 = 2× (zoomed in), 50 = ½× (zoomed out).
//     Zoom is clamped to [10, 1000] (10% – 10×) to prevent division by zero
//     and absurdly small viewports.
//   - The viewport in world-space has dimensions:
//       world_width  = camera.width  × 100 / zoom
//       world_height = camera.height × 100 / zoom
//   - The dirty flag is set to 1 at creation and whenever x, y, zoom, or
//     rotation change. It is cleared only by rt_camera_clear_dirty(). Renderers
//     that cache the camera transform should check is_dirty() each frame.
//   - Active bounds constrain the world-space viewport rectangle after
//     position, center, follow, movement, zoom, and bound changes.
//   - rt_camera_is_visible() projects all four entity corners and compares
//     their screen-space AABB with the viewport. A NULL camera pointer is
//     treated conservatively as always-visible.
//
// Ownership/Lifetime:
//   - Camera objects are reference-counted via rt_obj_new_i64 and have no
//     explicit public destroy function.
//   - Each parallax slot retains its Pixels handle. The camera finalizer
//     releases those references before the camera allocation is reclaimed.
//
// Links: src/runtime/graphics/2d/rt_camera.h (public API),
//        docs/zannalib/game.md (Camera section)
//
//===----------------------------------------------------------------------===//

#include "rt_camera.h"

#include "rt_graphics.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/// Maximum number of parallax scrolling layers per camera.
#define RT_CAMERA_MAX_PARALLAX 8
/// Maximum tile blits permitted while drawing one parallax layer.
#define RT_CAMERA_MAX_PARALLAX_TILES 65536

/// @brief A single parallax scrolling layer.
typedef struct {
    void *pixels;               ///< Pixels buffer to tile across the viewport
    void *cached_pixels;        ///< Prepared zoom/rotation tile retained between draws
    uint64_t cached_generation; ///< Source mutation generation used by cached_pixels
    int64_t cached_zoom;        ///< Zoom used by cached_pixels
    int64_t cached_rotation;    ///< Reduced rotation used by cached_pixels
    int64_t scroll_factor_x;    ///< X scroll % (100 = camera speed, 50 = half, 0 = static)
    int64_t scroll_factor_y;    ///< Y scroll % (100 = camera speed, 50 = half, 0 = static)
    int64_t offset_y;           ///< Vertical pixel offset for layer positioning
    int8_t active;              ///< 1 if this layer slot is in use
} rt_parallax_layer;

/// @brief Camera implementation structure.
typedef struct rt_camera_impl {
    int64_t x;          ///< Camera X position (world coordinates)
    int64_t y;          ///< Camera Y position (world coordinates)
    int64_t width;      ///< Viewport width
    int64_t height;     ///< Viewport height
    int64_t zoom;       ///< Zoom level (100 = 100%)
    int64_t rotation;   ///< Rotation in degrees
    int64_t has_bounds; ///< Whether bounds are set
    int64_t min_x;      ///< Minimum X bound
    int64_t min_y;      ///< Minimum Y bound
    int64_t max_x;      ///< Maximum X bound
    int64_t max_y;      ///< Maximum Y bound
    int64_t dirty;      ///< 1 if position/zoom/rotation changed since last rt_camera_clear_dirty
    int64_t deadzone_w; ///< Deadzone width (0 = disabled). Target within zone doesn't move camera.
    int64_t deadzone_h; ///< Deadzone height (0 = disabled).
    rt_parallax_layer parallax[RT_CAMERA_MAX_PARALLAX]; ///< Fixed parallax layer slots
    int64_t parallax_count;                             ///< Number of active layers
} rt_camera_impl;

/// @brief Validate-and-return a Camera pointer; NULL for NULL or wrong class.
/// @details Performs only validation; individual public APIs decide whether a
///          failed check traps, returns a fallback, or becomes a no-op.
/// @param camera_ptr Borrowed candidate Camera handle.
/// @return Borrowed implementation pointer, or `NULL` for null/wrong-class
///         handles.
static rt_camera_impl *camera_checked_or_null(void *camera_ptr) {
    if (!camera_ptr || !rt_obj_is_instance(camera_ptr, RT_CAMERA_CLASS_ID, sizeof(rt_camera_impl)))
        return NULL;
    return (rt_camera_impl *)camera_ptr;
}

/// @brief Release a GC-managed object held in `*slot` and NULL-out the slot.
/// @param slot Address of an owned nullable object handle. A valid heap payload
///        loses one reference and may be freed; the slot is always cleared.
static void camera_release_ref(void **slot) {
    if (!slot || !*slot)
        return;
    if (rt_heap_is_payload(*slot) && rt_obj_release_check0(*slot))
        rt_obj_free(*slot);
    *slot = NULL;
}

/// @brief Round a long double to the nearest int64, saturating at INT64_MIN/MAX instead of
/// overflowing.
/// @param value Finite floating-point value to convert.
/// @return Nearest int64 with ties away from zero, saturated at either limit.
static int64_t camera_ld_to_i64_sat(long double value) {
    if (isnan(value))
        return 0;
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)(value >= 0.0L ? value + 0.5L : value - 0.5L);
}

/// @brief Return the unsigned magnitude of an int64 without negating INT64_MIN.
static uint64_t camera_i64_magnitude(int64_t value) {
    return value < 0 ? UINT64_C(0) - (uint64_t)value : (uint64_t)value;
}

/// @brief Divide an unsigned 128-bit conceptual product using only uint64 operations.
/// @details The common path multiplies directly when it fits. The overflow path
///          accumulates quotient/remainder pairs by binary doubling, capping as
///          soon as the requested result limit is exceeded.
/// @param lhs First unsigned factor.
/// @param rhs Second unsigned factor.
/// @param divisor Positive divisor.
/// @param limit Largest quotient the caller can represent.
/// @param quotient Required exact floor quotient on success.
/// @param remainder Required exact remainder on success.
/// @return Nonzero when the floor quotient is at most @p limit.
static int camera_unsigned_mul_div(uint64_t lhs,
                                   uint64_t rhs,
                                   uint64_t divisor,
                                   uint64_t limit,
                                   uint64_t *quotient,
                                   uint64_t *remainder) {
    if (!quotient || !remainder || divisor == 0)
        return 0;
    if (lhs == 0 || rhs == 0) {
        *quotient = 0;
        *remainder = 0;
        return 1;
    }

    if (lhs <= UINT64_MAX / rhs) {
        uint64_t product = lhs * rhs;
        uint64_t q = product / divisor;
        if (q > limit)
            return 0;
        *quotient = q;
        *remainder = product % divisor;
        return 1;
    }

    uint64_t result_q = 0;
    uint64_t result_r = 0;
    uint64_t term_q = lhs / divisor;
    uint64_t term_r = lhs % divisor;
    const uint64_t capped = limit + 1u;

    while (rhs != 0) {
        if ((rhs & 1u) != 0u) {
            if (term_q > limit - result_q)
                return 0;
            result_q += term_q;

            if (result_r >= divisor - term_r) {
                result_r -= divisor - term_r;
                if (result_q == limit)
                    return 0;
                result_q++;
            } else {
                result_r += term_r;
            }
        }

        rhs >>= 1u;
        if (rhs == 0)
            break;

        uint64_t carry = 0;
        if (term_r >= divisor - term_r) {
            term_r -= divisor - term_r;
            carry = 1;
        } else {
            term_r += term_r;
        }

        if (term_q >= capped || term_q > (capped - carry) / 2u)
            term_q = capped;
        else
            term_q = term_q * 2u + carry;
    }

    *quotient = result_q;
    *remainder = result_r;
    return 1;
}

/// @brief Add two int64 values with saturation at INT64_MIN/MAX.
/// @param a Left operand.
/// @param b Right operand.
/// @return Saturating sum of @p a and @p b.
static int64_t camera_add_saturating(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Clamp @p value to the inclusive [min_value, max_value] range.
/// @param value Value to constrain.
/// @param min_value Inclusive lower bound.
/// @param max_value Inclusive upper bound.
/// @return @p value or the nearest bound; callers provide ordered bounds.
static int64_t camera_clamp_i64(int64_t value, int64_t min_value, int64_t max_value) {
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

/// @brief Subtract two int64 values with saturation at INT64_MIN/MAX.
/// @details Pure integer (not long double): the width of `long double` varies by
///   platform (80-bit x86, 64-bit MSVC, 128-bit AArch64), so a long-double
///   subtraction of two large int64 values is not exact everywhere and would make
///   camera positions differ across builds — at odds with VM/native determinism.
/// @param a Minuend.
/// @param b Subtrahend.
/// @return Saturating difference @p a minus @p b.
static int64_t camera_sub_saturating(int64_t a, int64_t b) {
    if (b < 0 && a > INT64_MAX + b)
        return INT64_MAX;
    if (b > 0 && a < INT64_MIN + b)
        return INT64_MIN;
    return a - b;
}

/// @brief Compute `round(value * mul / div)` with saturation; returns 0 on zero divisor.
/// @details Uses unsigned magnitude arithmetic and an overflow-only binary
///   quotient/remainder path. This avoids signed multiplication/addition
///   overflow, compiler-specific 128-bit helpers, and platform-dependent
///   long-double rounding.
/// @param value Value to scale.
/// @param mul Integer multiplier.
/// @param div Nonzero integer divisor for normal operation.
/// @return Rounded-half-away-from-zero quotient with int64 saturation, or `0`
///         when @p div is zero.
static int64_t camera_mul_div_saturating(int64_t value, int64_t mul, int64_t div) {
    if (div == 0)
        return 0;

    int negative = (value < 0) ^ (mul < 0) ^ (div < 0);
    uint64_t result_limit = negative ? (UINT64_C(1) << 63u) : (uint64_t)INT64_MAX;
    uint64_t divisor = camera_i64_magnitude(div);
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    if (!camera_unsigned_mul_div(camera_i64_magnitude(value),
                                 camera_i64_magnitude(mul),
                                 divisor,
                                 result_limit,
                                 &quotient,
                                 &remainder))
        return negative ? INT64_MIN : INT64_MAX;

    uint64_t round_threshold = divisor / 2u + divisor % 2u;
    if (remainder >= round_threshold) {
        if (quotient == result_limit)
            return negative ? INT64_MIN : INT64_MAX;
        quotient++;
    }

    if (!negative)
        return (int64_t)quotient;
    if (quotient == (UINT64_C(1) << 63u))
        return INT64_MIN;
    return -(int64_t)quotient;
}

/// @brief World-space width covered by the viewport at the current zoom (zoom is in percent).
/// @param camera Borrowed valid camera implementation.
/// @return Rounded, saturated `width * 100 / zoom`.
static int64_t camera_world_width(const rt_camera_impl *camera) {
    return camera_mul_div_saturating(camera->width, 100, camera->zoom);
}

/// @brief World-space height covered by the viewport at the current zoom.
/// @param camera Borrowed valid camera implementation.
/// @return Rounded, saturated `height * 100 / zoom`.
static int64_t camera_world_height(const rt_camera_impl *camera) {
    return camera_mul_div_saturating(camera->height, 100, camera->zoom);
}

/// @brief World-space X coordinate at the centre of the viewport.
/// @param camera Borrowed valid camera implementation.
/// @return Floating-point center derived from left edge and world-space width.
static double camera_center_x(const rt_camera_impl *camera) {
    return (double)camera->x + (double)camera_world_width(camera) * 0.5;
}

/// @brief World-space Y coordinate at the centre of the viewport.
/// @param camera Borrowed valid camera implementation.
/// @return Floating-point center derived from top edge and world-space height.
static double camera_center_y(const rt_camera_impl *camera) {
    return (double)camera->y + (double)camera_world_height(camera) * 0.5;
}

/// @brief Reduce a degree value before trigonometric evaluation.
static int64_t camera_rotation_for_math(int64_t degrees) {
    return degrees % 360;
}

/// @brief Precomputed forward/inverse transform values for one camera origin.
typedef struct {
    double center_x;
    double center_y;
    double screen_center_x;
    double screen_center_y;
    double scale;
    double inverse_scale;
    double cos_rotation;
    double sin_rotation;
} rt_camera_transform;

/// @brief Build a reusable transform for a camera-sized view at a supplied origin.
static void camera_transform_init(const rt_camera_impl *camera,
                                  int64_t origin_x,
                                  int64_t origin_y,
                                  rt_camera_transform *transform) {
    transform->center_x = (double)origin_x + (double)camera_world_width(camera) * 0.5;
    transform->center_y = (double)origin_y + (double)camera_world_height(camera) * 0.5;
    transform->screen_center_x = (double)camera->width * 0.5;
    transform->screen_center_y = (double)camera->height * 0.5;
    transform->scale = (double)camera->zoom / 100.0;
    transform->inverse_scale = 100.0 / (double)camera->zoom;
    double radians =
        -((double)camera_rotation_for_math(camera->rotation)) * 3.14159265358979323846 / 180.0;
    transform->cos_rotation = cos(radians);
    transform->sin_rotation = sin(radians);
}

/// @brief Apply a precomputed world-to-screen transform.
static void camera_apply_transform_cached(const rt_camera_transform *transform,
                                          double world_x,
                                          double world_y,
                                          double *screen_x,
                                          double *screen_y) {
    double dx = world_x - transform->center_x;
    double dy = world_y - transform->center_y;
    double rx = dx * transform->cos_rotation - dy * transform->sin_rotation;
    double ry = dx * transform->sin_rotation + dy * transform->cos_rotation;
    if (screen_x)
        *screen_x = rx * transform->scale + transform->screen_center_x;
    if (screen_y)
        *screen_y = ry * transform->scale + transform->screen_center_y;
}

/// @brief Apply a precomputed screen-to-world inverse transform.
static void camera_apply_inverse_transform_cached(const rt_camera_transform *transform,
                                                  double screen_x,
                                                  double screen_y,
                                                  double *world_x,
                                                  double *world_y) {
    double dx = (screen_x - transform->screen_center_x) * transform->inverse_scale;
    double dy = (screen_y - transform->screen_center_y) * transform->inverse_scale;
    double rx = dx * transform->cos_rotation + dy * transform->sin_rotation;
    double ry = -dx * transform->sin_rotation + dy * transform->cos_rotation;
    if (world_x)
        *world_x = rx + transform->center_x;
    if (world_y)
        *world_y = ry + transform->center_y;
}

/// @brief World → screen forward transform (translate, rotate, zoom, recenter).
/// @details Applies the camera's view transform: translate the world point by
///          minus the camera centre, rotate by the negated camera rotation
///          (so positive `rotation` rotates the *world* clockwise = camera
///          counter-clockwise), scale by zoom/100, then translate to screen
///          centre. Used by the parallax draw loop to place each tile.
/// @param camera Borrowed valid camera transform.
/// @param world_x Horizontal world coordinate.
/// @param world_y Vertical world coordinate.
/// @param screen_x Optional output for the horizontal screen coordinate.
/// @param screen_y Optional output for the vertical screen coordinate.
static void camera_apply_transform(const rt_camera_impl *camera,
                                   double world_x,
                                   double world_y,
                                   double *screen_x,
                                   double *screen_y) {
    rt_camera_transform transform;
    camera_transform_init(camera, camera->x, camera->y, &transform);
    camera_apply_transform_cached(&transform, world_x, world_y, screen_x, screen_y);
}

/// @brief Screen → world inverse transform.
/// @details Inverts `camera_apply_transform`: translate by minus screen
///          centre, scale by 100/zoom, rotate by the unnegated camera
///          rotation, then translate by camera centre. Used to compute
///          world-space tile coverage from screen corners during parallax
///          rendering.
/// @param camera Borrowed valid camera transform with nonzero zoom.
/// @param screen_x Horizontal screen coordinate.
/// @param screen_y Vertical screen coordinate.
/// @param world_x Optional output for the horizontal world coordinate.
/// @param world_y Optional output for the vertical world coordinate.
static void camera_apply_inverse_transform(const rt_camera_impl *camera,
                                           double screen_x,
                                           double screen_y,
                                           double *world_x,
                                           double *world_y) {
    rt_camera_transform transform;
    camera_transform_init(camera, camera->x, camera->y, &transform);
    camera_apply_inverse_transform_cached(&transform, screen_x, screen_y, world_x, world_y);
}

/// @brief Floor-division on int64 (rounds toward negative infinity, not toward zero).
/// @details C's `/` operator rounds toward zero, which produces the wrong
///          tile coordinate for negative world positions: `(-1)/16 == 0`
///          in C, but the tile containing `-1` is tile `-1`, not tile `0`.
///          The mismatch-of-signs check (`r != 0 && (sign(r) != sign(divisor))`)
///          decrements the quotient by one to recover floor semantics.
///          Used by the parallax tile loop to find the first/last tile
///          indices straddling the visible world bounds.
/// @param value Dividend.
/// @param divisor Positive nonzero tile dimension.
/// @return Mathematical floor of @p value divided by @p divisor.
static int64_t camera_floor_div(int64_t value, int64_t divisor) {
    int64_t q = value / divisor;
    int64_t r = value % divisor;
    if (r != 0 && ((r < 0) != (divisor < 0)))
        q--;
    return q;
}

/// @brief Compute the tile span from `first` to `last` (inclusive) and validate it is within
///        `RT_CAMERA_MAX_PARALLAX_TILES`. Writes the span count to `out_span` on success.
/// @param first Inclusive first tile index.
/// @param last Inclusive last tile index.
/// @param out_span Required output for the validated positive count.
/// @return `1` if the ordered span fits the parallax budget; otherwise `0`.
static int8_t camera_tile_span_within_limit(int64_t first, int64_t last, int64_t *out_span) {
    if (!out_span || last < first)
        return 0;
    uint64_t diff = (uint64_t)last - (uint64_t)first;
    if (diff >= (uint64_t)RT_CAMERA_MAX_PARALLAX_TILES)
        return 0;
    *out_span = (int64_t)(diff + 1u);
    return 1;
}

/// @brief Return 1 if `span_x * span_y` is within the `RT_CAMERA_MAX_PARALLAX_TILES` budget.
/// @details Divides rather than multiplies to avoid overflow on large span values.
/// @param span_x Positive horizontal tile count.
/// @param span_y Positive vertical tile count.
/// @return `1` when the product is positive and within budget; otherwise `0`.
static int8_t camera_tile_product_within_limit(int64_t span_x, int64_t span_y) {
    if (span_x <= 0 || span_y <= 0)
        return 0;
    return span_x <= RT_CAMERA_MAX_PARALLAX_TILES / span_y;
}

/// @brief Check that repeated positive stepping cannot overflow int64.
static int8_t camera_positive_step_span_fits(int64_t start, int64_t span, int64_t step) {
    if (span <= 0 || step <= 0)
        return 0;
    uint64_t distance_to_max = (uint64_t)INT64_MAX - (uint64_t)start;
    return (uint64_t)(span - 1) <= distance_to_max / (uint64_t)step;
}

/// @brief Compute the exact positive-step tile count needed to cover a viewport.
/// @param start First tile coordinate, normally in `[-tile_size + 1, 0]`.
/// @param viewport Exclusive positive coverage endpoint in pixels.
/// @param tile_size Positive tile dimension in pixels.
/// @return Required tile count, zero for invalid
///         dimensions, or a budget-exceeded sentinel.
static int64_t camera_covering_tile_span(int64_t start, int64_t viewport, int64_t tile_size) {
    if (viewport <= 0 || tile_size <= 0 || start >= viewport)
        return 0;
    uint64_t distance = (uint64_t)viewport - (uint64_t)start;
    uint64_t span = distance / (uint64_t)tile_size;
    if (distance % (uint64_t)tile_size != 0)
        span++;
    if (span > RT_CAMERA_MAX_PARALLAX_TILES)
        return RT_CAMERA_MAX_PARALLAX_TILES + 1;
    return (int64_t)span;
}

/// @brief Return a cached zoomed/rotated tile for one parallax layer.
/// @param layer Borrowed mutable active layer.
/// @param zoom Camera zoom percentage.
/// @param rotation Reduced camera rotation.
/// @return Borrowed source/cached Pixels handle, or `NULL` after allocation
///         failure. Existing stale cache storage is retained until replacement.
static void *camera_prepare_parallax_tile(rt_parallax_layer *layer,
                                          int64_t zoom,
                                          int64_t rotation) {
    if (zoom == 100 && rotation == 0)
        return layer->pixels;

    uint64_t generation = rt_pixels_generation(layer->pixels);
    if (layer->cached_pixels && layer->cached_generation == generation &&
        layer->cached_zoom == zoom && layer->cached_rotation == rotation)
        return layer->cached_pixels;

    void *scaled = NULL;
    void *rotated = NULL;
    void *tile_pixels = layer->pixels;
    if (zoom != 100) {
        int64_t scaled_w = camera_mul_div_saturating(rt_pixels_width(layer->pixels), zoom, 100);
        int64_t scaled_h = camera_mul_div_saturating(rt_pixels_height(layer->pixels), zoom, 100);
        if (scaled_w < 1)
            scaled_w = 1;
        if (scaled_h < 1)
            scaled_h = 1;
        scaled = rt_pixels_scale(layer->pixels, scaled_w, scaled_h);
        if (!scaled)
            return NULL;
        tile_pixels = scaled;
    }
    if (rotation != 0) {
        rotated = rt_pixels_rotate(tile_pixels, -(double)rotation);
        if (!rotated) {
            camera_release_ref(&scaled);
            return NULL;
        }
        tile_pixels = rotated;
    }

    if (rotated)
        camera_release_ref(&scaled);
    camera_release_ref(&layer->cached_pixels);
    layer->cached_pixels = tile_pixels;
    layer->cached_generation = generation;
    layer->cached_zoom = zoom;
    layer->cached_rotation = rotation;
    return layer->cached_pixels;
}

/// @brief Render one parallax layer at the camera's current zoom + rotation.
/// @details Three stages:
///          1. Build a *layer-specific* camera by scaling the parent
///             camera's position by the layer's per-axis `scroll_factor_*`
///             (a layer at factor 50 scrolls half as fast = appears
///             further away). The layer-camera retains the parent's
///             zoom/rotation so the layer transforms identically.
///          2. Pre-bake the tile pixels: scale to the camera's zoom (if
///             not 100), then rotate to the camera's negated rotation
///             (cancels the camera's view rotation when the tile is
///             drawn back through the forward transform below). Prepared
///             buffers are cached by source generation, zoom, and rotation.
///          3. Compute the world-space tile coverage by inverse-
///             transforming the four destination-canvas corners, then iterate every
///             tile in that AABB and `vgfx_blit_alpha` the prepared
///             pixels at the screen position from the forward transform.
///             Wrapping is implicit — tiles at any integer `(tx, ty)`
///             repeat the layer's source texture.
///          The final blit is centre-anchored, so the tile's centre of
///          rotation matches the precomputed rotated buffer's centre.
/// @param camera Borrowed valid parent camera.
/// @param layer Borrowed active parallax layer with a valid Pixels handle.
/// @param canvas Borrowed destination canvas.
/// @return `1` after tiling the layer; `0` for invalid dimensions, transform
///         allocation failure, or an excessive tile budget.
static int64_t camera_draw_parallax_transformed(const rt_camera_impl *camera,
                                                rt_parallax_layer *layer,
                                                void *canvas,
                                                int64_t coverage_width,
                                                int64_t coverage_height) {
    int64_t layer_x = camera_mul_div_saturating(camera->x, layer->scroll_factor_x, 100);
    int64_t layer_y = camera_add_saturating(
        camera_mul_div_saturating(camera->y, layer->scroll_factor_y, 100), layer->offset_y);
    rt_camera_transform transform;
    camera_transform_init(camera, layer_x, layer_y, &transform);

    int64_t pw = rt_pixels_width(layer->pixels);
    int64_t ph = rt_pixels_height(layer->pixels);
    if (pw <= 0 || ph <= 0)
        return 0;

    int64_t rotation = camera_rotation_for_math(camera->rotation);
    void *tile_pixels = camera_prepare_parallax_tile(layer, camera->zoom, rotation);
    if (!tile_pixels)
        return 0;

    int64_t draw_w = rt_pixels_width(tile_pixels);
    int64_t draw_h = rt_pixels_height(tile_pixels);
    double world_x = 0.0;
    double world_y = 0.0;
    double min_world_x = 0.0;
    double max_world_x = 0.0;
    double min_world_y = 0.0;
    double max_world_y = 0.0;

    camera_apply_inverse_transform_cached(&transform, 0.0, 0.0, &min_world_x, &min_world_y);
    max_world_x = min_world_x;
    max_world_y = min_world_y;

    camera_apply_inverse_transform_cached(
        &transform, (double)coverage_width, 0.0, &world_x, &world_y);
    if (world_x < min_world_x)
        min_world_x = world_x;
    if (world_x > max_world_x)
        max_world_x = world_x;
    if (world_y < min_world_y)
        min_world_y = world_y;
    if (world_y > max_world_y)
        max_world_y = world_y;

    camera_apply_inverse_transform_cached(
        &transform, 0.0, (double)coverage_height, &world_x, &world_y);
    if (world_x < min_world_x)
        min_world_x = world_x;
    if (world_x > max_world_x)
        max_world_x = world_x;
    if (world_y < min_world_y)
        min_world_y = world_y;
    if (world_y > max_world_y)
        max_world_y = world_y;

    camera_apply_inverse_transform_cached(
        &transform, (double)coverage_width, (double)coverage_height, &world_x, &world_y);
    if (world_x < min_world_x)
        min_world_x = world_x;
    if (world_x > max_world_x)
        max_world_x = world_x;
    if (world_y < min_world_y)
        min_world_y = world_y;
    if (world_y > max_world_y)
        max_world_y = world_y;

    int64_t first_tile_x =
        camera_floor_div(camera_sub_saturating(camera_ld_to_i64_sat(floorl(min_world_x)), pw), pw);
    int64_t last_tile_x =
        camera_floor_div(camera_add_saturating(camera_ld_to_i64_sat(floorl(max_world_x)), pw), pw);
    int64_t first_tile_y =
        camera_floor_div(camera_sub_saturating(camera_ld_to_i64_sat(floorl(min_world_y)), ph), ph);
    int64_t last_tile_y =
        camera_floor_div(camera_add_saturating(camera_ld_to_i64_sat(floorl(max_world_y)), ph), ph);

    int64_t span_x = 0;
    int64_t span_y = 0;
    if (!camera_tile_span_within_limit(first_tile_x, last_tile_x, &span_x) ||
        !camera_tile_span_within_limit(first_tile_y, last_tile_y, &span_y) ||
        !camera_tile_product_within_limit(span_x, span_y)) {
        return 0;
    }

    for (int64_t yi = 0; yi < span_y; ++yi) {
        int64_t ty = first_tile_y + yi;
        for (int64_t xi = 0; xi < span_x; ++xi) {
            int64_t tx = first_tile_x + xi;
            double screen_x = 0.0;
            double screen_y = 0.0;
            camera_apply_transform_cached(
                &transform,
                (double)camera_mul_div_saturating(tx, pw, 1) + (double)pw * 0.5,
                (double)camera_mul_div_saturating(ty, ph, 1) + (double)ph * 0.5,
                &screen_x,
                &screen_y);
            rt_canvas_blit_alpha(
                canvas,
                camera_ld_to_i64_sat((long double)screen_x - (long double)draw_w * 0.5L),
                camera_ld_to_i64_sat((long double)screen_y - (long double)draw_h * 0.5L),
                tile_pixels);
        }
    }

    return 1;
}

/// @brief Release all resources held by a parallax layer and mark it inactive.
/// @param layer Borrowed layer slot to clear; null/inactive slots are ignored.
static void camera_release_parallax_layer(rt_parallax_layer *layer) {
    if (!layer || !layer->active)
        return;
    camera_release_ref(&layer->cached_pixels);
    camera_release_ref(&layer->pixels);
    layer->cached_generation = 0;
    layer->cached_zoom = 0;
    layer->cached_rotation = 0;
    layer->scroll_factor_x = 0;
    layer->scroll_factor_y = 0;
    layer->offset_y = 0;
    layer->active = 0;
}

/// @brief GC finalizer: release all parallax layers before the camera allocation is freed.
/// @param obj Camera object being finalized; invalid handles are ignored.
static void camera_finalize(void *obj) {
    rt_camera_impl *camera = camera_checked_or_null(obj);
    if (!camera)
        return;
    for (int i = 0; i < RT_CAMERA_MAX_PARALLAX; i++)
        camera_release_parallax_layer(&camera->parallax[i]);
    camera->parallax_count = 0;
}

/// @brief Clamp camera position to bounds.
/// @details Converts the maximum right/bottom extent to the largest legal
///          viewport origin using the current zoomed world dimensions. Bounds
///          smaller than the viewport collapse the corresponding origin to
///          the minimum.
/// @param camera Borrowed valid camera; unchanged when bounds are disabled.
static void camera_clamp_bounds(rt_camera_impl *camera) {
    if (!camera->has_bounds)
        return;

    int64_t max_x = camera_sub_saturating(camera->max_x, camera_world_width(camera));
    int64_t max_y = camera_sub_saturating(camera->max_y, camera_world_height(camera));
    if (max_x < camera->min_x)
        max_x = camera->min_x;
    if (max_y < camera->min_y)
        max_y = camera->min_y;

    if (camera->x < camera->min_x)
        camera->x = camera->min_x;
    if (camera->y < camera->min_y)
        camera->y = camera->min_y;
    if (camera->x > max_x)
        camera->x = max_x;
    if (camera->y > max_y)
        camera->y = max_y;
}

//=============================================================================
// Camera Creation
//=============================================================================

/// @brief Construct a new Camera2D bound to a viewport size in pixels.
/// @details Width and height below 1 are clamped to 1 to avoid divide-by-zero
///          in `camera_world_width` / `_height`. Initial position is the
///          world origin, zoom is 100 (1×), rotation is 0, no bounds. The
///          camera starts marked dirty so the first render computes its
///          view transform fresh. Returns NULL on allocation failure.
/// @param width Viewport width in screen pixels; nonpositive values become one.
/// @param height Viewport height in screen pixels; nonpositive values become one.
/// @return Owned reference-counted Camera handle, or `NULL` on allocation
///         failure.
void *rt_camera_new(int64_t width, int64_t height) {
    if (width <= 0)
        width = 1;
    if (height <= 0)
        height = 1;

    rt_camera_impl *camera =
        (rt_camera_impl *)rt_obj_new_i64(RT_CAMERA_CLASS_ID, (int64_t)sizeof(rt_camera_impl));
    if (!camera)
        return NULL;

    camera->x = 0;
    camera->y = 0;
    camera->width = width;
    camera->height = height;
    camera->zoom = 100;
    camera->rotation = 0;
    camera->has_bounds = 0;
    camera->min_x = 0;
    camera->min_y = 0;
    camera->max_x = 0;
    camera->max_y = 0;
    camera->dirty = 1; /* newly created camera is always dirty */
    camera->deadzone_w = 0;
    camera->deadzone_h = 0;
    camera->parallax_count = 0;
    memset(camera->parallax, 0, sizeof(camera->parallax));
    rt_obj_set_finalizer(camera, camera_finalize);

    return camera;
}

//=============================================================================
// Camera Properties
//=============================================================================

/// @brief Read the camera viewport's world-space left edge. Traps on null.
/// @param camera_ptr Borrowed Camera handle.
/// @return Left-edge coordinate, or `0` if validation fails and the trap returns.
int64_t rt_camera_get_x(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.X: null camera");
        return 0;
    }
    return camera->x;
}

/// @brief Set the camera viewport's world-space left edge (clamped to active bounds, if any). Marks
/// the view-transform dirty so the next render recomputes derived state.
/// @param camera_ptr Borrowed Camera handle.
/// @param x Requested world-space left edge.
void rt_camera_set_x(void *camera_ptr, int64_t x) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.X: null camera");
        return;
    }
    int64_t old_x = camera->x;
    camera->x = x;
    camera_clamp_bounds(camera);
    if (camera->x != old_x)
        camera->dirty = 1;
}

/// @brief Read the camera viewport's world-space top edge. Traps on null.
/// @param camera_ptr Borrowed Camera handle.
/// @return Top-edge coordinate, or `0` if validation fails and the trap returns.
int64_t rt_camera_get_y(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.Y: null camera");
        return 0;
    }
    return camera->y;
}

/// @brief Set the camera viewport's world-space top edge (clamped to active bounds, if any).
/// @details Marks the transform dirty only when the final clamped value changes.
/// @param camera_ptr Borrowed Camera handle.
/// @param y Requested world-space top edge.
void rt_camera_set_y(void *camera_ptr, int64_t y) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.Y: null camera");
        return;
    }
    int64_t old_y = camera->y;
    camera->y = y;
    camera_clamp_bounds(camera);
    if (camera->y != old_y)
        camera->dirty = 1;
}

/// @brief Read the zoom level (100 = 1.0×, 200 = 2.0× zoom-in, 50 = 0.5× zoom-out).
/// @param camera_ptr Borrowed Camera handle.
/// @return Stored zoom percentage, or the neutral value `100` after a failed
///         validation trap.
int64_t rt_camera_get_zoom(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.Zoom: null camera");
        return 100;
    }
    return camera->zoom;
}

/// @brief Set zoom level (clamped to [10, 1000] = 0.1×–10×). Marks dirty and re-clamps to bounds.
/// @param camera_ptr Borrowed Camera handle.
/// @param zoom Requested integer percentage.
void rt_camera_set_zoom(void *camera_ptr, int64_t zoom) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.Zoom: null camera");
        return;
    }
    if (zoom < 10)
        zoom = 10;
    if (zoom > 1000)
        zoom = 1000;
    int64_t old_zoom = camera->zoom;
    int64_t old_x = camera->x;
    int64_t old_y = camera->y;
    camera->zoom = zoom;
    camera_clamp_bounds(camera);
    if (camera->zoom != old_zoom || camera->x != old_x || camera->y != old_y)
        camera->dirty = 1;
}

/// @brief Read the camera's rotation in degrees (positive = counter-clockwise).
/// @param camera_ptr Borrowed Camera handle.
/// @return Stored, unnormalized degree value, or `0` after a failed validation
///         trap.
int64_t rt_camera_get_rotation(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.Rotation: null camera");
        return 0;
    }
    return camera->rotation;
}

/// @brief Set rotation in degrees. No clamping (full ±360+ range allowed; renders modulo 360).
/// @param camera_ptr Borrowed Camera handle.
/// @param degrees Unnormalized integer camera angle.
void rt_camera_set_rotation(void *camera_ptr, int64_t degrees) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.Rotation: null camera");
        return;
    }
    if (camera->rotation != degrees) {
        camera->rotation = degrees;
        camera->dirty = 1;
    }
}

/// @brief Read the camera viewport width in pixels (set on construction).
/// @param camera_ptr Borrowed Camera handle.
/// @return Positive viewport width, or `0` after a failed validation trap.
int64_t rt_camera_get_width(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.Width: null camera");
        return 0;
    }
    return camera->width;
}

/// @brief Read the camera viewport height in pixels (set on construction).
/// @param camera_ptr Borrowed Camera handle.
/// @return Positive viewport height, or `0` after a failed validation trap.
int64_t rt_camera_get_height(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.Height: null camera");
        return 0;
    }
    return camera->height;
}

/// @brief Read the world-space X coordinate at the center of the viewport.
/// @details Uses the rounded world-space viewport width and saturating addition.
/// @param camera_ptr Borrowed Camera handle.
/// @return Integer center coordinate, or `0` after a failed validation trap.
int64_t rt_camera_get_center_x(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.CenterX: null camera");
        return 0;
    }
    return camera_add_saturating(camera->x, camera_world_width(camera) / 2);
}

/// @brief Read the world-space Y coordinate at the center of the viewport.
/// @details Uses the rounded world-space viewport height and saturating addition.
/// @param camera_ptr Borrowed Camera handle.
/// @return Integer center coordinate, or `0` after a failed validation trap.
int64_t rt_camera_get_center_y(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.CenterY: null camera");
        return 0;
    }
    return camera_add_saturating(camera->y, camera_world_height(camera) / 2);
}

/// @brief Place the viewport so the supplied world point is centered on screen.
/// @details Computes the requested origin with saturating subtraction, applies
///          active bounds, and marks dirty only if the final origin changes.
/// @param camera_ptr Borrowed Camera handle.
/// @param x Requested world-space center X.
/// @param y Requested world-space center Y.
void rt_camera_set_center(void *camera_ptr, int64_t x, int64_t y) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.SetCenter: null camera");
        return;
    }
    int64_t old_x = camera->x;
    int64_t old_y = camera->y;
    camera->x = camera_sub_saturating(x, camera_world_width(camera) / 2);
    camera->y = camera_sub_saturating(y, camera_world_height(camera) / 2);
    camera_clamp_bounds(camera);
    if (camera->x != old_x || camera->y != old_y)
        camera->dirty = 1;
}

//=============================================================================
// Camera Methods
//=============================================================================

/// @brief Snap the camera so (x, y) is at the viewport center. Re-clamps to bounds, marks dirty.
/// Use `_smooth_follow` for non-jarring tracking.
/// @param camera_ptr Borrowed Camera handle.
/// @param x Target world-space center X.
/// @param y Target world-space center Y.
void rt_camera_follow(void *camera_ptr, int64_t x, int64_t y) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.Follow: null camera");
        return;
    }

    int64_t old_x = camera->x;
    int64_t old_y = camera->y;
    camera->x = camera_sub_saturating(x, camera_world_width(camera) / 2);
    camera->y = camera_sub_saturating(y, camera_world_height(camera) / 2);
    camera_clamp_bounds(camera);
    if (camera->x != old_x || camera->y != old_y)
        camera->dirty = 1;
}

/// @brief Lerp the camera toward (target_x, target_y). `lerp_pct` is 0..1000 (0 = no move,
/// 1000 = instant snap). When a deadzone is set, no movement happens while the target stays
/// inside it — useful for platformer-style "loose" tracking.
/// @details Values at least 1000 snap; positive smaller values use rounded
///          thousandths and make one-pixel progress when rounding yields zero. Nonpositive
///          values do not follow, though existing bounds are still applied.
///          Dirty is set only when the final origin changes.
/// @param camera_ptr Borrowed Camera handle.
/// @param target_x Target world-space center X.
/// @param target_y Target world-space center Y.
/// @param lerp_pct Follow fraction in thousandths.
void rt_camera_smooth_follow(void *camera_ptr,
                             int64_t target_x,
                             int64_t target_y,
                             int64_t lerp_pct) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.SmoothFollow: null camera");
        return;
    }

    // Desired camera position (center target in viewport)
    int64_t desired_x = camera_sub_saturating(target_x, camera_world_width(camera) / 2);
    int64_t desired_y = camera_sub_saturating(target_y, camera_world_height(camera) / 2);

    // Deadzone: skip if target is within deadzone of current position. A zero axis
    // means "no deadzone on that axis" (always considered inside), so e.g.
    // SetDeadzone(64, 0) gives horizontal slack with tight vertical tracking —
    // requiring BOTH axes inside would make a single-axis deadzone unsatisfiable and
    // silently disable the whole thing.
    if (camera->deadzone_w > 0 || camera->deadzone_h > 0) {
        int64_t dx = camera_sub_saturating(desired_x, camera->x);
        int64_t dy = camera_sub_saturating(desired_y, camera->y);
        uint64_t distance_x = camera_i64_magnitude(dx);
        uint64_t distance_y = camera_i64_magnitude(dy);
        uint64_t radius_x = ((uint64_t)camera->deadzone_w - 1u) / 2u;
        uint64_t radius_y = ((uint64_t)camera->deadzone_h - 1u) / 2u;
        int8_t inside_x = camera->deadzone_w <= 0 || distance_x <= radius_x;
        int8_t inside_y = camera->deadzone_h <= 0 || distance_y <= radius_y;
        if (inside_x && inside_y)
            return;
    }

    int64_t old_x = camera->x;
    int64_t old_y = camera->y;

    // Lerp toward desired position. lerp_pct: 0-1000 (1000 = instant)
    if (lerp_pct >= 1000) {
        camera->x = desired_x;
        camera->y = desired_y;
    } else if (lerp_pct > 0) {
        int64_t dx = camera_sub_saturating(desired_x, camera->x);
        int64_t dy = camera_sub_saturating(desired_y, camera->y);
        int64_t step_x = camera_mul_div_saturating(dx, lerp_pct, 1000);
        int64_t step_y = camera_mul_div_saturating(dy, lerp_pct, 1000);
        // Make one-pixel progress when rounding produces zero; snapping the full
        // remainder here turns a tiny lerp into an unexpected instant jump.
        if (step_x == 0 && dx != 0)
            step_x = dx > 0 ? 1 : -1;
        if (step_y == 0 && dy != 0)
            step_y = dy > 0 ? 1 : -1;
        camera->x = camera_add_saturating(camera->x, step_x);
        camera->y = camera_add_saturating(camera->y, step_y);
    }

    camera_clamp_bounds(camera);
    if (camera->x != old_x || camera->y != old_y)
        camera->dirty = 1;
}

/// @brief Set the rectangular deadzone (centered on current position) in which `_smooth_follow`
/// is a no-op. Negative values are clamped to 0 (deadzone disabled).
/// @details Each axis is independent; a zero axis imposes no deadzone test.
///          This setting does not affect the dirty flag. Invalid handles are
///          silently ignored.
/// @param camera_ptr Borrowed Camera handle.
/// @param w Deadzone width in world units; nonpositive disables horizontal slack.
/// @param h Deadzone height in world units; nonpositive disables vertical slack.
void rt_camera_set_deadzone(void *camera_ptr, int64_t w, int64_t h) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return;
    camera->deadzone_w = w > 0 ? w : 0;
    camera->deadzone_h = h > 0 ? h : 0;
}

/// @brief Project a world-space point into screen-space pixels, applying zoom, rotation, and
/// translation. Outputs are written through `screen_x` / `screen_y` (rounded). Useful for HUD
/// markers anchored to world entities.
/// @details Invalid handles or null output pointers leave both outputs unchanged.
/// @param camera_ptr Borrowed Camera handle.
/// @param world_x Horizontal world coordinate.
/// @param world_y Vertical world coordinate.
/// @param screen_x Required output receiving rounded, saturated screen X.
/// @param screen_y Required output receiving rounded, saturated screen Y.
void rt_camera_world_to_screen(
    void *camera_ptr, int64_t world_x, int64_t world_y, int64_t *screen_x, int64_t *screen_y) {
    if (!camera_ptr || !screen_x || !screen_y)
        return;

    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return;
    double sx = 0.0, sy = 0.0;
    camera_apply_transform(camera, (double)world_x, (double)world_y, &sx, &sy);
    *screen_x = camera_ld_to_i64_sat((long double)sx);
    *screen_y = camera_ld_to_i64_sat((long double)sy);
}

/// @brief One-axis convenience: project just the X component of a world point. Y is implicitly
/// the camera's vertical center so rotation contributes correctly.
/// @param camera_ptr Borrowed Camera handle.
/// @param world_x Horizontal world coordinate.
/// @return Rounded, saturated screen X; invalid cameras return @p world_x.
int64_t rt_camera_to_screen_x(void *camera_ptr, int64_t world_x) {
    if (!camera_ptr)
        return world_x;
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return world_x;
    double sx = 0.0;
    camera_apply_transform(camera, (double)world_x, camera_center_y(camera), &sx, NULL);
    return camera_ld_to_i64_sat((long double)sx);
}

/// @brief One-axis convenience: project just the Y component of a world point.
/// @details X is implicitly the camera's horizontal center.
/// @param camera_ptr Borrowed Camera handle.
/// @param world_y Vertical world coordinate.
/// @return Rounded, saturated screen Y; invalid cameras return @p world_y.
int64_t rt_camera_to_screen_y(void *camera_ptr, int64_t world_y) {
    if (!camera_ptr)
        return world_y;
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return world_y;
    double sy = 0.0;
    camera_apply_transform(camera, camera_center_x(camera), (double)world_y, NULL, &sy);
    return camera_ld_to_i64_sat((long double)sy);
}

/// @brief Inverse of `_world_to_screen`: turn a screen pixel into world coordinates. Useful
/// for hit-testing mouse clicks against world entities.
/// @details Invalid handles or null output pointers leave both outputs unchanged.
/// @param camera_ptr Borrowed Camera handle.
/// @param screen_x Horizontal screen coordinate.
/// @param screen_y Vertical screen coordinate.
/// @param world_x Required output receiving rounded, saturated world X.
/// @param world_y Required output receiving rounded, saturated world Y.
void rt_camera_screen_to_world(
    void *camera_ptr, int64_t screen_x, int64_t screen_y, int64_t *world_x, int64_t *world_y) {
    if (!camera_ptr || !world_x || !world_y)
        return;

    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return;
    double wx = 0.0, wy = 0.0;
    camera_apply_inverse_transform(camera, (double)screen_x, (double)screen_y, &wx, &wy);
    *world_x = camera_ld_to_i64_sat((long double)wx);
    *world_y = camera_ld_to_i64_sat((long double)wy);
}

/// @brief One-axis convenience: unproject just the X component of a screen pixel to world.
/// @details Screen Y is implicitly the viewport's vertical center.
/// @param camera_ptr Borrowed Camera handle.
/// @param screen_x Horizontal screen coordinate.
/// @return Rounded, saturated world X; invalid cameras return @p screen_x.
int64_t rt_camera_to_world_x(void *camera_ptr, int64_t screen_x) {
    if (!camera_ptr)
        return screen_x;
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return screen_x;
    double wx = 0.0;
    camera_apply_inverse_transform(
        camera, (double)screen_x, (double)camera->height * 0.5, &wx, NULL);
    return camera_ld_to_i64_sat((long double)wx);
}

/// @brief One-axis convenience: unproject just the Y component of a screen pixel to world.
/// @details Screen X is implicitly the viewport's horizontal center.
/// @param camera_ptr Borrowed Camera handle.
/// @param screen_y Vertical screen coordinate.
/// @return Rounded, saturated world Y; invalid cameras return @p screen_y.
int64_t rt_camera_to_world_y(void *camera_ptr, int64_t screen_y) {
    if (!camera_ptr)
        return screen_y;
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return screen_y;
    double wy = 0.0;
    camera_apply_inverse_transform(
        camera, (double)camera->width * 0.5, (double)screen_y, NULL, &wy);
    return camera_ld_to_i64_sat((long double)wy);
}

/// @brief Translate the camera by (dx, dy) world units and re-clamp to bounds.
/// @details Both additions saturate at the int64 limits.
/// @param camera_ptr Borrowed Camera handle.
/// @param dx Horizontal world-space displacement.
/// @param dy Vertical world-space displacement.
void rt_camera_move(void *camera_ptr, int64_t dx, int64_t dy) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.Move: null camera");
        return;
    }
    int64_t old_x = camera->x;
    int64_t old_y = camera->y;
    camera->x = camera_add_saturating(old_x, dx);
    camera->y = camera_add_saturating(old_y, dy);
    camera_clamp_bounds(camera);
    if (camera->x != old_x || camera->y != old_y)
        camera->dirty = 1;
}

/// @brief Constrain camera position to stay within the rectangle [(min_x, min_y), (max_x,
/// max_y)]. The current position is immediately clamped. Use to prevent the camera from showing
/// "outside the level".
/// @details The maximum coordinates describe the right/bottom world extent,
///          not the maximum viewport origin. Reversed or undersized bounds
///          collapse the corresponding origin to its minimum. Dirty is set
///          only if immediate clamping changes the current position.
/// @param camera_ptr Borrowed Camera handle.
/// @param min_x Minimum viewport left edge.
/// @param min_y Minimum viewport top edge.
/// @param max_x Maximum world-space right extent.
/// @param max_y Maximum world-space bottom extent.
void rt_camera_set_bounds(
    void *camera_ptr, int64_t min_x, int64_t min_y, int64_t max_x, int64_t max_y) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.SetBounds: null camera");
        return;
    }
    camera->has_bounds = 1;
    camera->min_x = min_x;
    camera->min_y = min_y;
    camera->max_x = max_x;
    camera->max_y = max_y;
    int64_t old_x = camera->x;
    int64_t old_y = camera->y;
    camera_clamp_bounds(camera);
    if (camera->x != old_x || camera->y != old_y)
        camera->dirty = 1;
}

/// @brief Disable bounds clamping. Camera can be moved freely afterwards.
/// @details Does not move the camera or set its dirty flag.
/// @param camera_ptr Borrowed Camera handle.
void rt_camera_clear_bounds(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera) {
        rt_trap("Camera.ClearBounds: null camera");
        return;
    }
    camera->has_bounds = 0;
}

//=============================================================================
// Visibility Culling
//=============================================================================

/// @brief Test whether a world-space rectangle has any overlap with the camera viewport.
/// Useful as a cheap broad-phase cull before drawing each entity. Conservative: rotation is
/// handled by projecting the four corners and checking the screen-space AABB.
/// @details Rectangles with nonpositive dimensions are invisible. Touching only
///          a viewport boundary is not considered overlap. A null pointer is
///          conservatively visible, while a non-null wrong-class handle is not.
/// @param camera_ptr Borrowed Camera handle; may be `NULL`.
/// @param x Rectangle left edge in world coordinates.
/// @param y Rectangle top edge in world coordinates.
/// @param w Positive rectangle width.
/// @param h Positive rectangle height.
/// @return `1` when the projected AABB overlaps the viewport; otherwise `0`.
int64_t rt_camera_is_visible(void *camera_ptr, int64_t x, int64_t y, int64_t w, int64_t h) {
    if (!camera_ptr)
        return 1; // Null camera — conservatively treat as visible
    if (w <= 0 || h <= 0)
        return 0;
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return 0;

    rt_camera_transform transform;
    camera_transform_init(camera, camera->x, camera->y, &transform);
    int64_t right = camera_add_saturating(x, w);
    int64_t bottom = camera_add_saturating(y, h);
    double sx[4];
    double sy[4];
    camera_apply_transform_cached(&transform, (double)x, (double)y, &sx[0], &sy[0]);
    camera_apply_transform_cached(&transform, (double)right, (double)y, &sx[1], &sy[1]);
    camera_apply_transform_cached(&transform, (double)x, (double)bottom, &sx[2], &sy[2]);
    camera_apply_transform_cached(&transform, (double)right, (double)bottom, &sx[3], &sy[3]);

    double min_x = sx[0], max_x = sx[0];
    double min_y = sy[0], max_y = sy[0];
    for (int i = 1; i < 4; i++) {
        if (sx[i] < min_x)
            min_x = sx[i];
        if (sx[i] > max_x)
            max_x = sx[i];
        if (sy[i] < min_y)
            min_y = sy[i];
        if (sy[i] > max_y)
            max_y = sy[i];
    }

    if (max_x <= 0.0 || min_x >= (double)camera->width || max_y <= 0.0 ||
        min_y >= (double)camera->height)
        return 0;
    return 1;
}

//=============================================================================
// Dirty Flag — Enables callers to skip re-rendering when camera is stationary
//=============================================================================

/// @brief Returns 1 if the camera's transform has changed since the last `_clear_dirty`. Lets
/// callers skip costly re-renders when the view is stationary.
/// @param camera_ptr Borrowed Camera handle.
/// @return Current dirty flag, or `0` for invalid handles.
int64_t rt_camera_is_dirty(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return 0;
    return camera->dirty;
}

/// @brief Reset the dirty flag — call after rendering to acknowledge the latest transform.
/// @param camera_ptr Borrowed Camera handle; invalid handles are ignored.
void rt_camera_clear_dirty(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return;
    camera->dirty = 0;
}

//=============================================================================
// Parallax Layer Management
//=============================================================================

/// @brief Register a parallax background layer with `pixels` as its texture. `scroll_x_pct` and
/// `scroll_y_pct` are 0..100 (0 = stationary, 100 = scrolls 1:1 with camera). Higher numbers feel
/// "closer" to the viewer. Up to RT_CAMERA_MAX_PARALLAX layers; returns the slot index or -1.
/// @details Valid scroll factors are clamped to 0..100, the Pixels object is
///          retained, and the first inactive slot is used.
/// @param camera_ptr Borrowed Camera handle.
/// @param pixels Borrowed valid Pixels handle retained by the new layer.
/// @param scroll_x_pct Horizontal parallax percentage.
/// @param scroll_y_pct Vertical parallax percentage.
/// @return Slot index in 0..7, or `-1` for invalid arguments or a full camera.
int64_t rt_camera_add_parallax(void *camera_ptr,
                               void *pixels,
                               int64_t scroll_x_pct,
                               int64_t scroll_y_pct) {
    if (!camera_ptr || !pixels)
        return -1;
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return -1;
    if (!rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl)))
        return -1;
    if (camera->parallax_count >= RT_CAMERA_MAX_PARALLAX)
        return -1;

    for (int i = 0; i < RT_CAMERA_MAX_PARALLAX; i++) {
        if (!camera->parallax[i].active) {
            rt_obj_retain_maybe(pixels);
            camera->parallax[i].pixels = pixels;
            camera->parallax[i].scroll_factor_x = camera_clamp_i64(scroll_x_pct, 0, 100);
            camera->parallax[i].scroll_factor_y = camera_clamp_i64(scroll_y_pct, 0, 100);
            camera->parallax[i].offset_y = 0;
            camera->parallax[i].active = 1;
            camera->parallax_count++;
            return (int64_t)i;
        }
    }
    return -1;
}

/// @brief Remove a parallax layer by slot index (returned from `_add_parallax`). No-op if
/// the slot is already inactive or the index is out of range.
/// @param camera_ptr Borrowed Camera handle.
/// @param index Slot index to release.
void rt_camera_remove_parallax(void *camera_ptr, int64_t index) {
    if (!camera_ptr)
        return;
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return;
    if (index < 0 || index >= RT_CAMERA_MAX_PARALLAX)
        return;
    if (camera->parallax[index].active) {
        camera_release_parallax_layer(&camera->parallax[index]);
        camera->parallax_count--;
    }
}

/// @brief Remove all parallax layers and reset the layer count.
/// @param camera_ptr Borrowed Camera handle; invalid handles are ignored.
void rt_camera_clear_parallax(void *camera_ptr) {
    if (!camera_ptr)
        return;
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return;
    for (int i = 0; i < RT_CAMERA_MAX_PARALLAX; i++)
        camera_release_parallax_layer(&camera->parallax[i]);
    camera->parallax_count = 0;
}

/// @brief Number of currently-active parallax layers.
/// @param camera_ptr Borrowed Camera handle.
/// @return Active slot count, or `0` for invalid handles.
int64_t rt_camera_parallax_count(void *camera_ptr) {
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return 0;
    return camera->parallax_count;
}

/// @brief Render every active parallax layer to `canvas` at offsets computed from the camera's
/// current scroll position and each layer's scroll factor. Returns the number of layers drawn.
/// @details Layers render in ascending slot order. Neutral transforms tile
///          directly across the larger of canvas and viewport dimensions;
///          zoomed/rotated transforms prepare a temporary tile and cover the
///          inverse-transformed viewport. A per-layer tile budget prevents
///          excessive iteration. Layers skipped for invalid dimensions,
///          allocation failure, or budget overflow are not counted.
/// @param camera_ptr Borrowed Camera handle.
/// @param canvas Borrowed destination canvas.
/// @return Number of layers successfully tiled, or `0` for invalid arguments.
int64_t rt_camera_draw_parallax(void *camera_ptr, void *canvas) {
    if (!camera_ptr || !canvas)
        return 0;
    rt_camera_impl *camera = camera_checked_or_null(camera_ptr);
    if (!camera)
        return 0;

    int64_t layers_drawn = 0;
    int64_t coverage_width = rt_canvas_width(canvas);
    int64_t coverage_height = rt_canvas_height(canvas);
    if (coverage_width < camera->width)
        coverage_width = camera->width;
    if (coverage_height < camera->height)
        coverage_height = camera->height;
    int64_t rotation = camera_rotation_for_math(camera->rotation);

    for (int i = 0; i < RT_CAMERA_MAX_PARALLAX; i++) {
        rt_parallax_layer *layer = &camera->parallax[i];
        if (!layer->active || !layer->pixels)
            continue;

        int64_t pw = rt_pixels_width(layer->pixels);
        int64_t ph = rt_pixels_height(layer->pixels);

        if (pw <= 0 || ph <= 0)
            continue;

        if (camera->zoom != 100 || rotation != 0) {
            layers_drawn += camera_draw_parallax_transformed(
                camera, layer, canvas, coverage_width, coverage_height);
            continue;
        }

        /* Compute the parallax scroll offset */
        int64_t scroll_x = camera_mul_div_saturating(camera->x, layer->scroll_factor_x, 100);
        int64_t scroll_y = camera_add_saturating(
            camera_mul_div_saturating(camera->y, layer->scroll_factor_y, 100), layer->offset_y);

        /* Compute starting tile position (wrap negative modulo) */
        int64_t start_x = -(scroll_x % pw);
        if (start_x > 0)
            start_x -= pw;
        int64_t start_y = -(scroll_y % ph);
        if (start_y > 0)
            start_y -= ph;

        int64_t span_x = camera_covering_tile_span(start_x, coverage_width, pw);
        int64_t span_y = camera_covering_tile_span(start_y, coverage_height, ph);
        if (!camera_tile_product_within_limit(span_x, span_y))
            continue;

        if (!camera_positive_step_span_fits(start_x, span_x, pw) ||
            !camera_positive_step_span_fits(start_y, span_y, ph))
            continue;

        /* Counted loops cannot wrap at INT64_MAX. */
        int64_t ty = start_y;
        for (int64_t yi = 0; yi < span_y; ++yi) {
            int64_t tx = start_x;
            for (int64_t xi = 0; xi < span_x; ++xi) {
                rt_canvas_blit_alpha(canvas, tx, ty, layer->pixels);
                if (xi + 1 < span_x)
                    tx += pw;
            }
            if (yi + 1 < span_y)
                ty += ph;
        }

        layers_drawn++;
    }

    return layers_drawn;
}
