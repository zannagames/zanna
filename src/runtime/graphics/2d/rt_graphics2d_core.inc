//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_graphics2d_core.inc
/// @brief Shared 2D graphics class identifiers, private implementation types,
/// math helpers, reference helpers, blitters, samplers, and region-copy
/// utilities.
///
/// @details This include fragment is consumed only by rt_graphics2d.c. It
/// preserves the original declaration order and static helper visibility while
/// making the core utility layer independently navigable.
///
// File: src/runtime/graphics/2d/rt_graphics2d_core.inc
// Purpose: Shared 2D class ids, private impl structs, math helpers, reference helpers, blitters,
// samplers, and region-copy utilities.
//
// Key invariants:
//   - Included only by src/runtime/graphics/2d/rt_graphics2d.c.
//   - Keeps the original rt_graphics2d.c declaration order and static helper
//     visibility; this is a physical source split, not a behavior split.
//   - Public runtime entry points preserve their existing names and signatures.
//
// Ownership/Lifetime:
//   - Ownership rules are unchanged from the original monolithic source file;
//     GC finalizers release retained handles and heap buffers for each class.
//
// Links: src/runtime/graphics/2d/rt_graphics2d.c,
//        src/runtime/graphics/2d/rt_graphics2d_internal.h
//
//===----------------------------------------------------------------------===//

#define RT2D_MAX_DIM 1000000LL
#define RT2D_MAX_ELEMENTS 268435456LL
#define RT2D_PI 3.14159265358979323846
#define RT2D_RENDERTARGET_MAGIC 0x525432445247544cLL /* "RT2DRGTL" */
#define RT2D_TEXTURE_MAGIC 0x525432445445584cLL      /* "RT2DTEXL" */
#define RT2D_RENDERER_MAGIC 0x52543244524e444cLL     /* "RT2DRNDL" */
#define RT2D_RENDERTARGET_CLASS_ID INT64_C(-0x620100)
#define RT2D_TEXTURE_CLASS_ID INT64_C(-0x620101)
#define RT2D_RENDERER_CLASS_ID INT64_C(-0x620102)
#define RT2D_MATERIAL_CLASS_ID INT64_C(-0x620103)
#define RT2D_SHADER_CLASS_ID INT64_C(-0x620104)
#define RT2D_VIEWPORT_CLASS_ID INT64_C(-0x620105)
#define RT2D_PATH_CLASS_ID INT64_C(-0x62010A)
#define RT2D_TRANSFORM_CLASS_ID INT64_C(-0x620110)
#define RT2D_SAMPLER_CLASS_ID INT64_C(-0x620111)
#define RT2D_BLENDSTATE_CLASS_ID INT64_C(-0x620112)
#define RT2D_SPRITERENDERER_CLASS_ID INT64_C(-0x620113)
#define RT2D_TILECHUNKCACHE_CLASS_ID INT64_C(-0x620114)
#define RT2D_TILEMAPRENDERER_CLASS_ID INT64_C(-0x620115)
#define RT2D_ANIMATIONCLIP_CLASS_ID INT64_C(-0x620116)
#define RT2D_ANIMATEDSPRITE_CLASS_ID INT64_C(-0x620117)
#define RT2D_TEXTLAYOUT_CLASS_ID INT64_C(-0x620118)
#define RT2D_RENDERPASS_CLASS_ID INT64_C(-0x620119)
#define RT2D_RENDERGRAPH_CLASS_ID INT64_C(-0x62011A)
#define RT2D_COLLISIONMASK_CLASS_ID INT64_C(-0x62011B)
#define RT2D_HITBOX_CLASS_ID INT64_C(-0x62011C)
#define RT2D_PALETTE_CLASS_ID INT64_C(-0x62011D)
#define RT2D_GRADIENT_CLASS_ID INT64_C(-0x62011E)
#define RT2D_CAMERARIG_CLASS_ID INT64_C(-0x62011F)
#define RT2D_ASEPRITEIMPORTER_CLASS_ID INT64_C(-0x620120)
#define RT2D_TILEDMAPLOADER_CLASS_ID INT64_C(-0x620121)
#define RT2D_TEXTUREPACKERATLAS_CLASS_ID INT64_C(-0x620122)
#define RT2D_POSTPROCESS_CLASS_ID INT64_C(-0x620123)
#define RT2D_SURFACE_CLASS_ID INT64_C(-0x620124)
#define RT2D_GPUTEXTURE_CLASS_ID INT64_C(-0x620125)
#define RT2D_SCREENSCALER_CLASS_ID INT64_C(-0x620126)

/// @brief Normalizes a finite angle into one signed revolution.
/// @param angle_deg In-out angle in degrees.
/// @return `1` when @p angle_deg is non-null and finite, otherwise `0`.
/// @details Successful normalization uses `fmod(angle, 360)`, so positive
///          inputs produce `[0, 360)` and negative inputs produce `(-360, 0]`.
static int rt2d_normalize_angle_degrees(double *angle_deg) {
    if (!angle_deg || !isfinite(*angle_deg))
        return 0;
    *angle_deg = fmod(*angle_deg, 360.0);
    return isfinite(*angle_deg);
}

/// @brief Convert a finite floor-rounded double to int64_t without UB.
///
/// @details Rotated blits compute floating-point bounds before scanning the
///   destination pixels. C defines out-of-range floating-to-integer casts as
///   undefined behavior, so this helper rejects non-finite or unrepresentable
///   values before performing the cast.
///
/// @param value Source bound to floor.
/// @param out Receives the floor-rounded int64_t value on success.
/// @return 1 on success, 0 if @p value cannot be represented safely.
static int rt2d_floor_i64_checked(double value, int64_t *out) {
    if (!out || !isfinite(value) || value < (double)INT64_MIN || value >= (double)INT64_MAX)
        return 0;
    *out = (int64_t)floor(value);
    return 1;
}

/// @brief Convert a finite ceil-rounded double to int64_t without UB.
///
/// @details Companion to @c rt2d_floor_i64_checked for maximum bounds. The
///   explicit range check keeps sanitizer/cppcheck builds from seeing undefined
///   behavior when adversarial dimensions or pivots create enormous bounds.
///
/// @param value Source bound to ceil.
/// @param out Receives the ceil-rounded int64_t value on success.
/// @return 1 on success, 0 if @p value cannot be represented safely.
static int rt2d_ceil_i64_checked(double value, int64_t *out) {
    if (!out || !isfinite(value) || value < (double)INT64_MIN || value >= (double)INT64_MAX)
        return 0;
    *out = (int64_t)ceil(value);
    return 1;
}

typedef struct {
    int64_t magic;
    void *pixels;
} rt_rendertarget2d_impl;

typedef struct {
    int64_t magic;
    void *pixels;
    int64_t filter;
    int64_t wrap;
} rt_texture2d_impl;

typedef struct {
    void *source;
    int32_t source_kind;
    int64_t x;
    int64_t y;
    int64_t sx;
    int64_t sy;
    int64_t width;
    int64_t height;
    int64_t dst_width;
    int64_t dst_height;
    int64_t tint;
    int64_t alpha;
    int64_t blend_mode;
    int64_t sampler_filter;
    int64_t sampler_wrap;
    double angle_deg;
    int64_t pivot_x;
    int64_t pivot_y;
    int8_t has_rotation;
} rt_renderer2d_cmd;

typedef struct {
    int64_t magic;
    rt_renderer2d_cmd *cmds;
    int64_t count;
    int64_t capacity;
    int64_t active;
    int64_t tint;
    int64_t alpha;
    int64_t blend_mode;
} rt_renderer2d_impl;

typedef struct {
    int64_t tint;
    int64_t alpha;
    int64_t blend_mode;
} rt_material2d_impl;

typedef struct {
    int64_t effect;
    int64_t amount;
    int64_t color;
} rt_shader2d_impl;

typedef rt_shader2d_impl rt_postprocess2d_impl;

typedef struct {
    int64_t virtual_width;
    int64_t virtual_height;
    int64_t screen_width;
    int64_t screen_height;
    int64_t integer_scaling;
    int64_t scale;
    int64_t offset_x;
    int64_t offset_y;
} rt_viewport2d_impl;

typedef struct {
    int64_t x;
    int64_t y;
    int32_t move;
} rt_path2d_point;

typedef struct {
    rt_path2d_point *points;
    int64_t count;
    int64_t capacity;
} rt_path2d_impl;

//=============================================================================
// Internal helpers — shared by every class in this file
//=============================================================================

/// @brief Clamp an int64 to `[min, max]`.
/// @param value Value to constrain.
/// @param min Inclusive lower bound.
/// @param max Inclusive upper bound.
/// @return @p min or @p max when outside the interval, otherwise @p value.
static int64_t clamp_i64(int64_t value, int64_t min, int64_t max) {
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

/// @brief Clamp to the unsigned-byte range `[0, 255]`. Used for RGBA channel math
///        that passes through int64 intermediates to avoid overflow on multiplies.
/// @param value Integer channel or alpha value to constrain.
/// @return @p value clamped to `0..255`.
static int64_t clamp_u8_i64(int64_t value) {
    return clamp_i64(value, 0, 255);
}

/// @brief Saturating integer addition: returns INT64_MAX / INT64_MIN instead of wrapping.
/// @param a Left operand.
/// @param b Right operand.
/// @return `a + b`, saturated at the signed 64-bit limits.
int64_t rt2d_saturating_add_i64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Saturating integer subtraction: returns INT64_MAX / INT64_MIN instead of wrapping.
/// @param a Minuend.
/// @param b Subtrahend.
/// @return `a - b`, saturated at the signed 64-bit limits.
static int64_t saturating_sub_i64(int64_t a, int64_t b) {
    if (b >= 0)
        return (a < INT64_MIN + b) ? INT64_MIN : a - b;
    return (a > INT64_MAX + b) ? INT64_MAX : a - b;
}

/// @brief Saturating integer multiplication with exact integer overflow guards.
/// @details Division-based checks avoid depending on `long double` precision.
///          This matters on Windows, where `long double` commonly has the same
///          53-bit mantissa as `double` and cannot distinguish adjacent large
///          int64 values.
/// @param a Left operand.
/// @param b Right operand.
/// @return `a * b`, saturated at the signed 64-bit limits.
int64_t rt2d_saturating_mul_i64(int64_t a, int64_t b) {
    if (a > 0) {
        if (b > 0 && a > INT64_MAX / b)
            return INT64_MAX;
        if (b < 0 && b < INT64_MIN / a)
            return INT64_MIN;
    } else if (a < 0) {
        if (b > 0 && a < INT64_MIN / b)
            return INT64_MIN;
        if (b < 0 && a < INT64_MAX / b)
            return INT64_MAX;
    }
    return a * b;
}

/// @brief Tests an opaque handle against either of two compatible runtime classes.
/// @param obj Opaque object handle to validate.
/// @param primary_class_id Primary accepted runtime class identifier.
/// @param alias_class_id Alternate accepted runtime class identifier.
/// @param min_size Minimum payload bytes required for either class.
/// @return Nonzero when @p obj belongs to either class with sufficient storage.
static int32_t rt2d_is_instance_of_either(void *obj,
                                          int64_t primary_class_id,
                                          int64_t alias_class_id,
                                          size_t min_size) {
    return obj && (rt_obj_is_instance(obj, primary_class_id, min_size) ||
                   rt_obj_is_instance(obj, alias_class_id, min_size));
}

/// @brief Validates and casts a render-target or surface handle.
/// @param target Opaque RenderTarget2D or Surface2D candidate.
/// @return The implementation pointer when class, payload size, and magic
///         cookie match; otherwise `NULL`.
static rt_rendertarget2d_impl *rendertarget2d_checked(void *target) {
    if (!target)
        return NULL;
    if (!rt2d_is_instance_of_either(target,
                                    RT2D_RENDERTARGET_CLASS_ID,
                                    RT2D_SURFACE_CLASS_ID,
                                    sizeof(rt_rendertarget2d_impl)))
        return NULL;
    rt_rendertarget2d_impl *impl = (rt_rendertarget2d_impl *)target;
    return impl->magic == RT2D_RENDERTARGET_MAGIC && impl->pixels &&
                   rt_obj_is_instance(impl->pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl))
               ? impl
               : NULL;
}

/// @brief Validate and cast a `void *` to `rt_texture2d_impl *` by checking the magic cookie.
/// @param texture Opaque Texture2D or GpuTexture2D candidate.
/// @return The validated implementation pointer, or `NULL`.
static rt_texture2d_impl *texture2d_checked(void *texture) {
    if (!texture)
        return NULL;
    if (!rt2d_is_instance_of_either(
            texture, RT2D_TEXTURE_CLASS_ID, RT2D_GPUTEXTURE_CLASS_ID, sizeof(rt_texture2d_impl)))
        return NULL;
    rt_texture2d_impl *impl = (rt_texture2d_impl *)texture;
    return impl->magic == RT2D_TEXTURE_MAGIC && impl->pixels &&
                   rt_obj_is_instance(impl->pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl))
               ? impl
               : NULL;
}

/// @brief Validate and cast a `void *` to `rt_renderer2d_impl *` by checking the magic cookie.
/// @param renderer Opaque Renderer2D candidate.
/// @return The validated implementation pointer, or `NULL`.
static rt_renderer2d_impl *renderer2d_checked(void *renderer) {
    if (!renderer)
        return NULL;
    if (!rt_obj_is_instance(renderer, RT2D_RENDERER_CLASS_ID, sizeof(rt_renderer2d_impl)))
        return NULL;
    rt_renderer2d_impl *impl = (rt_renderer2d_impl *)renderer;
    if (impl->magic != RT2D_RENDERER_MAGIC || impl->count < 0 || impl->capacity < 0 ||
        impl->count > impl->capacity || (impl->capacity > 0 && !impl->cmds))
        return NULL;
    return impl;
}

/// @brief Test whether @p obj is non-NULL and has the runtime class id @p class_id.
/// @details Used by every public Renderer2D / Material2D / TileLayer2D entry
///          point as a one-line type guard before downcasting an opaque
///          handle to its concrete impl pointer.
/// @param obj Opaque object handle to test.
/// @param class_id Required runtime class identifier.
/// @return Nonzero when @p obj is non-null and belongs to @p class_id.
int32_t rt2d_has_class(void *obj, int64_t class_id) {
    return obj && rt_obj_is_instance(obj, class_id, 0);
}

/// @brief Like rt2d_has_class but also requires the payload be at least
///        @p min_size bytes (guards struct-layout mismatches before a cast).
/// @param obj Opaque object handle to test.
/// @param class_id Required runtime class identifier.
/// @param min_size Minimum object payload size in bytes.
/// @return Nonzero when both the class and payload-size requirements match.
int32_t rt2d_has_class_min(void *obj, int64_t class_id, size_t min_size) {
    return obj && rt_obj_is_instance(obj, class_id, min_size);
}

/// @brief Tests whether a handle is a supported bitmap-backed font class.
/// @param font Opaque font candidate.
/// @return Nonzero for BitmapFont or SpriteFont instances, otherwise zero.
int32_t rt2d_is_bitmap_font_handle(void *font) {
    return font && (rt_obj_is_instance(font, RT_BITMAPFONT_CLASS_ID, 0) ||
                    rt_obj_is_instance(font, RT_SPRITEFONT_CLASS_ID, 0));
}

/// @brief Safe-cast an opaque handle to rt_shader2d_impl, or NULL if not one.
/// @param shader Opaque Shader2D candidate.
/// @return The validated implementation pointer, or `NULL`.
static rt_shader2d_impl *shader2d_checked(void *shader) {
    return rt2d_has_class_min(shader, RT2D_SHADER_CLASS_ID, sizeof(rt_shader2d_impl))
               ? (rt_shader2d_impl *)shader
               : NULL;
}

/// @brief Safe-cast an opaque handle to rt_postprocess2d_impl, or NULL.
/// @param postprocess Opaque PostProcess2D candidate.
/// @return The validated implementation pointer, or `NULL`.
static rt_postprocess2d_impl *postprocess2d_checked(void *postprocess) {
    return rt2d_has_class_min(postprocess, RT2D_POSTPROCESS_CLASS_ID, sizeof(rt_postprocess2d_impl))
               ? (rt_postprocess2d_impl *)postprocess
               : NULL;
}

/// @brief True if @p shader is either a Shader2D or a PostProcess2D (both
///        accept the same apply-to-pixels usage).
/// @param shader Opaque effect handle to test.
/// @return Nonzero for either supported effect class, otherwise zero.
static int32_t rt2d_is_shader_like(void *shader) {
    return shader2d_checked(shader) || postprocess2d_checked(shader);
}

/// @brief Safe-cast an opaque handle to rt_viewport2d_impl, or NULL.
/// @param viewport Opaque Viewport2D or ScreenScaler candidate.
/// @return The validated implementation pointer, or `NULL`.
static rt_viewport2d_impl *viewport2d_checked(void *viewport) {
    return rt2d_is_instance_of_either(viewport,
                                      RT2D_VIEWPORT_CLASS_ID,
                                      RT2D_SCREENSCALER_CLASS_ID,
                                      sizeof(rt_viewport2d_impl))
               ? (rt_viewport2d_impl *)viewport
               : NULL;
}

/// @brief Tests whether a value lies in a half-open signed interval.
/// @details Uses an unsigned subtraction trick to collapse the "negative start" and
///   "value < start" cases into a single bounds check without signed-overflow UB.
/// @param start Inclusive interval start.
/// @param length Positive interval length.
/// @param value Candidate coordinate.
/// @return Nonzero exactly when @p value is in `[start, start + length)`.
static int32_t point_in_interval_i64(int64_t start, int64_t length, int64_t value) {
    if (length <= 0 || value < start)
        return 0;
    return ((uint64_t)value - (uint64_t)start) < (uint64_t)length;
}

/// @brief Test whether two intervals `[a_start, a_start+a_length)` and `[b_start,
/// b_start+b_length)` overlap.
/// @details Implemented as "does either interval contain the other's start?" — one containment
/// implies
///   overlap in both directions. Zero-length intervals never overlap.
/// @param a_start Start of the first interval.
/// @param a_length Length of the first interval.
/// @param b_start Start of the second interval.
/// @param b_length Length of the second interval.
/// @return Nonzero when the half-open intervals overlap.
static int32_t intervals_overlap_i64(int64_t a_start,
                                     int64_t a_length,
                                     int64_t b_start,
                                     int64_t b_length) {
    if (a_length <= 0 || b_length <= 0)
        return 0;
    return point_in_interval_i64(a_start, a_length, b_start) ||
           point_in_interval_i64(b_start, b_length, a_start);
}

/// @brief Return the unsigned distance from @p value to zero (i.e. `abs(value)` as uint64).
/// @details Negative values use `(uint64_t)(-(value+1))+1u` so INT64_MIN is handled
///          without signed-overflow UB.
/// @param value Signed value whose magnitude is requested.
/// @return The exact unsigned magnitude, including `2^63` for `INT64_MIN`.
static uint64_t distance_to_zero_i64(int64_t value) {
    if (value >= 0)
        return (uint64_t)value;
    return (uint64_t)(-(value + 1)) + 1u;
}

/// @brief Clip a blit operation along one axis, adjusting destination, source, and length in place.
/// @details Handles the four cases where the destination or source position is negative or runs
/// past
///   the limit: computes the largest offset `skip` that makes both positions non-negative, advances
///   both positions by `skip`, reduces `length` accordingly, then clips `length` to the remaining
///   space in both buffers. Sets `*length = 0` and returns 0 on any out-of-bounds condition. Called
///   twice per blit (once for X, once for Y) so the blit loop never has to range-check coordinates.
/// @param dst_pos In-out destination coordinate on the selected axis.
/// @param src_pos In-out source coordinate on the selected axis.
/// @param length In-out copy length on the selected axis.
/// @param dst_limit Exclusive destination dimension.
/// @param src_limit Exclusive source dimension.
/// @return Nonzero when clipping leaves a positive in-bounds span.
static int32_t blit_clip_axis(
    int64_t *dst_pos, int64_t *src_pos, int64_t *length, int64_t dst_limit, int64_t src_limit) {
    if (!dst_pos || !src_pos || !length || *length <= 0 || dst_limit <= 0 || src_limit <= 0) {
        if (length)
            *length = 0;
        return 0;
    }
    if ((*dst_pos >= 0 && *dst_pos >= dst_limit) || (*src_pos >= 0 && *src_pos >= src_limit)) {
        *length = 0;
        return 0;
    }

    uint64_t skip = 0;
    if (*dst_pos < 0)
        skip = distance_to_zero_i64(*dst_pos);
    if (*src_pos < 0) {
        uint64_t src_skip = distance_to_zero_i64(*src_pos);
        if (src_skip > skip)
            skip = src_skip;
    }
    if (skip > 0) {
        if (skip >= (uint64_t)*length) {
            *length = 0;
            return 0;
        }
        if (*dst_pos >= 0 && skip >= (uint64_t)(dst_limit - *dst_pos)) {
            *length = 0;
            return 0;
        }
        if (*src_pos >= 0 && skip >= (uint64_t)(src_limit - *src_pos)) {
            *length = 0;
            return 0;
        }
        int64_t offset = (int64_t)skip;
        *dst_pos += offset;
        *src_pos += offset;
        *length -= offset;
    }
    if (*dst_pos < 0 || *src_pos < 0 || *dst_pos >= dst_limit || *src_pos >= src_limit) {
        *length = 0;
        return 0;
    }

    int64_t dst_remaining = dst_limit - *dst_pos;
    int64_t src_remaining = src_limit - *src_pos;
    if (*length > dst_remaining)
        *length = dst_remaining;
    if (*length > src_remaining)
        *length = src_remaining;
    return *length > 0;
}

/// @brief Round a `long double` to the nearest `int64_t`, saturating at INT64_MIN/INT64_MAX.
/// @details Used for viewport coordinate transforms where the intermediate result must
///   be in `long double` to avoid losing precision, but must ultimately become an integer
///   pixel coordinate. Non-finite values are clamped to the appropriate extreme.
/// @param value Floating-point coordinate to convert.
/// @return The nearest signed 64-bit integer, saturated on overflow; negative
///         infinity maps to `INT64_MIN`, while positive infinity and NaN map to
///         `INT64_MAX`.
static int64_t round_long_double_to_i64(long double value) {
    if (!isfinite((double)value))
        return value < 0.0L ? INT64_MIN : INT64_MAX;
    if (value >= (long double)INT64_MAX)
        return INT64_MAX;
    if (value <= (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)(value >= 0.0L ? value + 0.5L : value - 0.5L);
}

/// @brief Clamp a dimension to `[1, RT2D_MAX_DIM]`. Zero / negative requests become
///        1 so nothing downstream has to special-case an empty row or column.
/// @param value Requested dimension.
/// @return @p value clamped to the supported positive dimension range.
static int64_t normalized_dim(int64_t value) {
    return clamp_i64(value, 1, RT2D_MAX_DIM);
}

/// @brief Validate and compute `width * height` for buffer allocation, guarding
///        against integer overflow and hard dimension limits.
/// @details Performs three separate overflow checks in sequence:
///            1. `width > RT2D_MAX_DIM || height > RT2D_MAX_DIM` — reject pathological
///               dimensions before even multiplying.
///            2. `width > INT64_MAX / height` — catch the `width * height` overflow.
///            3. byte-count checks against both `INT64_MAX` and `SIZE_MAX` —
///               reject requests that cannot be represented by the allocator
///               on a 32-bit host.
///          The result is also bounded by `RT2D_MAX_ELEMENTS` (currently 256Mi) so
///          the allocator never sees a request larger than the design ceiling even
///          when the math doesn't overflow. Returns 0 on any failure so the caller
///          fails the allocation cleanly rather than truncating silently.
/// @param width Requested width in pixels / cells.
/// @param height Requested height in pixels / cells.
/// @param elem_size Bytes per element (e.g. `sizeof(uint32_t)` for RGBA32).
/// @param out_count Optional output — receives `width * height` on success.
/// @return 1 on success, 0 on invalid dimensions or overflow.
int32_t rt2d_checked_count(int64_t width, int64_t height, int64_t elem_size, int64_t *out_count) {
    if (width <= 0 || height <= 0 || elem_size <= 0)
        return 0;
    if (width > RT2D_MAX_DIM || height > RT2D_MAX_DIM)
        return 0;
    if (width > INT64_MAX / height)
        return 0;
    int64_t count = width * height;
    if (count > RT2D_MAX_ELEMENTS)
        return 0;
    if (count > INT64_MAX / elem_size)
        return 0;
    if ((uint64_t)elem_size > (uint64_t)SIZE_MAX ||
        (uint64_t)count > (uint64_t)SIZE_MAX / (size_t)elem_size)
        return 0;
    if (out_count)
        *out_count = count;
    return 1;
}

/// @brief Pick an initial backing-array capacity for a dynamic list.
/// @param requested Caller-requested entry count.
/// @return `RT2D_INITIAL_CAP` for non-positive requests, `1048576` for larger
///         requests, and the positive request unchanged otherwise.
/// @details The default avoids tiny allocations when callers do not specify a
///          capacity, while explicitly requested small positive capacities are
///          preserved.
int64_t rt2d_initial_capacity(int64_t requested) {
    if (requested <= 0)
        return RT2D_INITIAL_CAP;
    if (requested > 1048576)
        return 1048576;
    return requested;
}

/// @brief Retain a heap-payload object slot, no-op for non-payload handles.
/// @details Object handles in this runtime can be either real GC-managed payloads
///          or opaque handles (constants, interned values) that don't need refcount
///          maintenance. `rt_heap_is_payload` distinguishes the two; this wrapper
///          means callers don't have to probe the heap bit themselves.
/// @param obj Borrowed object handle whose payload reference count is incremented.
void rt2d_retain_ref(void *obj) {
    if (obj && rt_heap_is_payload(obj))
        rt_obj_retain_maybe(obj);
}

/// @brief Release one reference through a slot and zero the slot atomically.
/// @details Used by every `_finalize` and every "replace slot" setter in this file.
///          The slot is cleared *before* the potential free so observers can't see
///          a dangling pointer mid-release. Non-payload handles are skipped just
///          like `rt2d_retain_ref`.
/// @param slot In-out pointer slot.  A non-null contained object is released as
///        appropriate and the slot is always cleared first.
void rt2d_release_ref_slot(void **slot) {
    if (!slot || !*slot)
        return;
    void *obj = *slot;
    *slot = NULL;
    if (rt_heap_is_payload(obj) && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Normalize a user-supplied color to `0x00RRGGBB` form.
/// @details Shape/debug APIs accept Canvas-style `0x00RRGGBB`, raw
///          `0xRRGGBBAA`, or tagged Color.RGBA values. Alpha is ignored by
///          these RGB-only drawing primitives.
/// @param color Packed RGB, packed RGBA, or explicitly tagged Color value.
/// @return The normalized low 24-bit `0x00RRGGBB` color.
int64_t rt2d_draw_rgb(int64_t color) {
    uint64_t full = (uint64_t)color;
    uint64_t c = full & 0xFFFFFFFFu;
    if ((full & (uint64_t)RT_PIXELS_COLOR_EXPLICIT_ALPHA_FLAG) != 0)
        return (int64_t)(c & 0x00FFFFFFu);
    if (c <= 0x00FFFFFFu)
        return (int64_t)c;
    return (int64_t)((c >> 8) & 0x00FFFFFFu);
}

/// @brief Apply a tint multiplier and alpha scale to a single `0xRRGGBBAA` texel.
/// @details Tint is a `0x00RRGGBB` multiplicative color (each channel ∈ [0,1]
///          scaled to [0,255]); alpha is a scalar in [0,255]. A negative tint
///          skips the multiply — used by callers that want pure alpha scaling
///          without touching color. Integer-math rounding uses the standard
///          `(a * b + 127) / 255` bias so channel products match the "divide by
///          255" behaviour you'd expect from floating-point.
/// @param rgba Source texel encoded as `0xRRGGBBAA`.
/// @param tint Multiplicative `0x00RRGGBB` tint, or a negative value to skip tinting.
/// @param alpha Additional alpha scale, clamped to `0..255`.
/// @return The transformed `0xRRGGBBAA` texel.
static uint32_t apply_tint_alpha(uint32_t rgba, int64_t tint, int64_t alpha) {
    int64_t r = (rgba >> 24) & 255;
    int64_t g = (rgba >> 16) & 255;
    int64_t b = (rgba >> 8) & 255;
    int64_t a = rgba & 255;
    if (tint >= 0) {
        int64_t tr = (tint >> 16) & 255;
        int64_t tg = (tint >> 8) & 255;
        int64_t tb = tint & 255;
        r = (r * tr + 127) / 255;
        g = (g * tg + 127) / 255;
        b = (b * tb + 127) / 255;
    }
    a = (a * clamp_u8_i64(alpha) + 127) / 255;
    return (uint32_t)((r << 24) | (g << 16) | (b << 8) | a);
}

/// @brief Composite a source texel over a destination texel using the given blend
///        mode, returning the final `0xRRGGBBAA` result.
/// @details Three modes:
///          - **OPAQUE** (or `src.a == 255`): source replaces destination wholesale,
///            so the fully-opaque fast path skips the per-channel math entirely.
///          - **ADD** (additive): source is scaled by its own alpha, then added to
///            destination with per-channel clamp at 255. Destination alpha rises
///            using the standard over-style `a + sa - a*sa/255` formula so adding
///            onto an existing alpha surface doesn't reset it.
///          - **ALPHA** (default): straight-alpha Porter-Duff source-over.
///          All multiplies use the `(a * b + 127) / 255` rounding bias so results
///          match float-reference output within 1 ULP.
/// @param dst Destination texel encoded as `0xRRGGBBAA`.
/// @param src Source texel encoded as `0xRRGGBBAA`.
/// @param blend_mode Requested `RT_GRAPHICS2D_BLEND_*` operation; values other
///        than opaque and additive use source-over alpha blending.
/// @return The composited `0xRRGGBBAA` texel.
static uint32_t blend_pixel(uint32_t dst, uint32_t src, int64_t blend_mode) {
    int64_t sr = (src >> 24) & 255;
    int64_t sg = (src >> 16) & 255;
    int64_t sb = (src >> 8) & 255;
    int64_t sa = src & 255;
    int64_t dr = (dst >> 24) & 255;
    int64_t dg = (dst >> 16) & 255;
    int64_t db = (dst >> 8) & 255;
    int64_t da = dst & 255;

    if (blend_mode == RT_GRAPHICS2D_BLEND_OPAQUE)
        return src;

    if (blend_mode == RT_GRAPHICS2D_BLEND_ADD) {
        int64_t r = clamp_u8_i64(dr + (sr * sa + 127) / 255);
        int64_t g = clamp_u8_i64(dg + (sg * sa + 127) / 255);
        int64_t b = clamp_u8_i64(db + (sb * sa + 127) / 255);
        int64_t a = clamp_u8_i64(da + sa - (da * sa + 127) / 255);
        return (uint32_t)((r << 24) | (g << 16) | (b << 8) | a);
    }

    if (sa >= 255)
        return src;

    return rt_pixels_alpha_over_rgba(dst, src);
}

/// @brief Copy a region from `src` to `dst` with tint + alpha + blend mode, clipping
///        both source and destination to their respective bounds.
/// @details The workhorse behind every texture / sprite / nine-slice draw in this
///          file. Clipping is done up front in four passes (negative source x/y,
///          negative dest x/y, over-right source, over-right dest) — each pass
///          trims the copy rectangle AND compensates the opposite rectangle so the
///          remaining area stays aligned. Fully-transparent source pixels
///          (`alpha == 0` after tint+alpha application) are skipped entirely to
///          avoid a wasted `rt_pixels_set` round-trip.
///
///          Does NOT validate that tint / alpha / blend_mode are in their defined
///          enum ranges — clamping happens upstream in the public API setters, not
///          here. Keeps the inner loop branch-light.
/// @param dst Destination Pixels (written in place).
/// @param dx Destination top-left X coordinate.
/// @param dy Destination top-left Y coordinate.
/// @param src Source Pixels (read-only).
/// @param sx Source top-left X coordinate; may be negative and is clipped.
/// @param sy Source top-left Y coordinate; may be negative and is clipped.
/// @param width Requested copy width; clipped to both images.
/// @param height Requested copy height; clipped to both images.
/// @param tint 0x00RRGGBB multiplicative tint, or negative to skip tinting.
/// @param alpha Alpha scale in [0, 255].
/// @param blend_mode One of `RT_GRAPHICS2D_BLEND_*`.
void rt2d_blit_pixels(void *dst,
                      int64_t dx,
                      int64_t dy,
                      void *src,
                      int64_t sx,
                      int64_t sy,
                      int64_t width,
                      int64_t height,
                      int64_t tint,
                      int64_t alpha,
                      int64_t blend_mode) {
    if (!dst || !src || width <= 0 || height <= 0)
        return;

    int64_t dst_width = rt_pixels_width(dst);
    int64_t dst_height = rt_pixels_height(dst);
    int64_t src_width = rt_pixels_width(src);
    int64_t src_height = rt_pixels_height(src);

    if (!blit_clip_axis(&dx, &sx, &width, dst_width, src_width) ||
        !blit_clip_axis(&dy, &sy, &height, dst_height, src_height))
        return;

    void *snapshot = NULL;
    void *read_src = src;
    void *read_dst = dst;
    if (dst == src &&
        !(dx + width <= sx || sx + width <= dx || dy + height <= sy || sy + height <= dy)) {
        snapshot = rt_pixels_clone(src);
        if (!snapshot)
            return;
        read_src = snapshot;
        read_dst = snapshot;
    }

    /* Coordinates are pre-clipped to both buffers by blit_clip_axis above, so walk
     * raw row pointers instead of paying a validated rt_pixels_get/set (each of
     * which re-runs a class check + bounds test) per texel — this is the workhorse
     * for every Renderer2D/RenderTarget draw. The single pixels_touch at the end
     * preserves the generation bump that rt_pixels_set would have done per pixel,
     * so alpha-scan / sprite / GPU-upload caches still invalidate. */
    rt_pixels_impl *si = rt_pixels_checked_impl_or_null(read_src);
    rt_pixels_impl *dri = rt_pixels_checked_impl_or_null(read_dst);
    rt_pixels_impl *dwi = rt_pixels_checked_impl_or_null(dst);
    if (si && si->data && dri && dri->data && dwi && dwi->data) {
        int8_t changed = 0;
        for (int64_t y = 0; y < height; y++) {
            const uint32_t *srow = &si->data[(sy + y) * si->width + sx];
            const uint32_t *drow = &dri->data[(dy + y) * dri->width + dx];
            uint32_t *wrow = &dwi->data[(dy + y) * dwi->width + dx];
            for (int64_t x = 0; x < width; x++) {
                uint32_t source = apply_tint_alpha(srow[x], tint, alpha);
                if (blend_mode != RT_GRAPHICS2D_BLEND_OPAQUE && (source & 255u) == 0)
                    continue;
                uint32_t result = (uint32_t)blend_pixel(drow[x], source, blend_mode);
                if (wrow[x] != result) {
                    wrow[x] = result;
                    changed = 1;
                }
            }
        }
        if (changed)
            pixels_touch(dwi);
    } else {
        /* Defensive fallback (unreachable for valid handles): per-pixel validated. */
        for (int64_t y = 0; y < height; y++) {
            for (int64_t x = 0; x < width; x++) {
                uint32_t source = apply_tint_alpha(
                    (uint32_t)rt_pixels_get(read_src, sx + x, sy + y), tint, alpha);
                if (blend_mode != RT_GRAPHICS2D_BLEND_OPAQUE && (source & 255u) == 0)
                    continue;
                uint32_t dest = (uint32_t)rt_pixels_get(read_dst, dx + x, dy + y);
                rt_pixels_set(dst, dx + x, dy + y, (int64_t)blend_pixel(dest, source, blend_mode));
            }
        }
    }

    rt2d_release_ref_slot(&snapshot);
}

/// @brief Wrap or clamp a 1D source-pixel coordinate per the requested wrap mode.
/// @details RT_GRAPHICS2D_WRAP_REPEAT performs a positive-modulo wrap (so
///          negative coords land in [0, limit)); other wrap modes clamp into
///          [0, limit-1]. Used by both nearest- and linear-filter samplers.
/// @param coord Source coordinate to resolve.
/// @param limit Positive coordinate-space extent.
/// @param wrap Requested `RT_GRAPHICS2D_WRAP_*` mode.
/// @return A coordinate in [0, limit). Returns 0 for limit <= 0.
static int64_t sampler_coord_i64(int64_t coord, int64_t limit, int64_t wrap) {
    if (limit <= 0)
        return 0;
    if (wrap == RT_GRAPHICS2D_WRAP_REPEAT) {
        int64_t wrapped = coord % limit;
        return wrapped < 0 ? wrapped + limit : wrapped;
    }
    if (coord < 0)
        return 0;
    if (coord >= limit)
        return limit - 1;
    return coord;
}

/// @brief Resolve a source coordinate inside a texture-region span.
/// @details Linear filtering for atlas regions must never borrow texels from
///          neighboring regions. The local coordinate is therefore wrapped or
///          clamped against the requested span first, then translated by the
///          region origin and finally resolved against the backing Pixels
///          bounds using the texture wrap mode.
/// @param origin Source-space origin of the texture region.
/// @param local_coord Coordinate relative to @p origin.
/// @param span Region extent used for local wrapping or clamping.
/// @param source_limit Full backing-image extent.
/// @param wrap Requested `RT_GRAPHICS2D_WRAP_*` mode.
/// @return A valid backing-image coordinate, or `0` for a degenerate extent.
static int64_t sampler_region_coord_i64(
    int64_t origin, int64_t local_coord, int64_t span, int64_t source_limit, int64_t wrap) {
    if (source_limit <= 0 || span <= 0)
        return 0;
    int64_t local = 0;
    if (wrap == RT_GRAPHICS2D_WRAP_REPEAT) {
        local = local_coord % span;
        if (local < 0)
            local += span;
    } else {
        local = clamp_i64(local_coord, 0, span - 1);
    }
    return sampler_coord_i64(rt2d_saturating_add_i64(origin, local), source_limit, wrap);
}

/// @brief Round and clamp a long-double color channel to a 0..255 byte.
/// @param value Floating-point channel value.
/// @return The nearest byte value, clamped to `0..255`.
static uint8_t clamp_channel_ld(long double value) {
    if (value <= 0.0L)
        return 0;
    if (value >= 255.0L)
        return 255;
    return (uint8_t)(value + 0.5L);
}

/// @brief Alpha-premultiply one color channel of a packed RGBA value.
/// @param rgba  Packed 0xRRGGBBAA pixel.
/// @param shift Bit shift selecting the channel (24=R, 16=G, 8=B).
/// @return channel * alpha / 255 as a long double (premultiplied channel).
static long double premul_channel(uint32_t rgba, int shift) {
    long double c = (long double)((rgba >> shift) & 255u);
    long double a = (long double)(rgba & 255u);
    return c * a / 255.0L;
}

/// @brief Bilinearly interpolate four RGBA texels in premultiplied-alpha space.
/// @details Weights the four corner texels by (tx, ty), blends in
///          alpha-premultiplied space (correct edge behaviour against
///          transparent texels), then un-premultiplies back to straight RGBA.
///          Returns fully transparent (0) when the blended alpha is zero.
/// @param c00 Top-left texel encoded as `0xRRGGBBAA`.
/// @param c10 Top-right texel encoded as `0xRRGGBBAA`.
/// @param c01 Bottom-left texel encoded as `0xRRGGBBAA`.
/// @param c11 Bottom-right texel encoded as `0xRRGGBBAA`.
/// @param tx Fractional horizontal position in `[0, 1]`.
/// @param ty Fractional vertical position in `[0, 1]`.
/// @return The interpolated 0xRRGGBBAA color.
static uint32_t bilerp_premul_rgba(
    uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, long double tx, long double ty) {
    long double w00 = (1.0L - tx) * (1.0L - ty);
    long double w10 = tx * (1.0L - ty);
    long double w01 = (1.0L - tx) * ty;
    long double w11 = tx * ty;
    long double a = (long double)(c00 & 255u) * w00 + (long double)(c10 & 255u) * w10 +
                    (long double)(c01 & 255u) * w01 + (long double)(c11 & 255u) * w11;
    uint32_t alpha = clamp_channel_ld(a);
    if (alpha == 0)
        return 0;

    long double r_pm = premul_channel(c00, 24) * w00 + premul_channel(c10, 24) * w10 +
                       premul_channel(c01, 24) * w01 + premul_channel(c11, 24) * w11;
    long double g_pm = premul_channel(c00, 16) * w00 + premul_channel(c10, 16) * w10 +
                       premul_channel(c01, 16) * w01 + premul_channel(c11, 16) * w11;
    long double b_pm = premul_channel(c00, 8) * w00 + premul_channel(c10, 8) * w10 +
                       premul_channel(c01, 8) * w01 + premul_channel(c11, 8) * w11;
    long double scale = 255.0L / a;
    uint32_t r = clamp_channel_ld(r_pm * scale);
    uint32_t g = clamp_channel_ld(g_pm * scale);
    uint32_t b = clamp_channel_ld(b_pm * scale);
    return (r << 24) | (g << 16) | (b << 8) | alpha;
}

/// @brief Sample one pixel from @p src using nearest-neighbor filtering.
/// @details Maps the destination position (dst_x, dst_y) within a
///          (dst_span_w × dst_span_h) blit region back to a source position
///          inside the (src_span_w × src_span_h) source region anchored at
///          (sx, sy). Picks the nearest source texel, applying the wrap mode
///          for out-of-range coordinates. Returns 0 (transparent black) on
///          degenerate input.
/// @param src Borrowed source Pixels image.
/// @param sx Source-region origin X.
/// @param sy Source-region origin Y.
/// @param src_span_w Source-region width.
/// @param src_span_h Source-region height.
/// @param dst_span_w Full destination mapping width.
/// @param dst_span_h Full destination mapping height.
/// @param dst_x X coordinate relative to the destination mapping.
/// @param dst_y Y coordinate relative to the destination mapping.
/// @param wrap Source-coordinate wrap mode.
/// @return The sampled `0xRRGGBBAA` texel, or transparent black for degenerate input.
static uint32_t sample_pixels_nearest(const rt_pixels_impl *src,
                                      int64_t sx,
                                      int64_t sy,
                                      int64_t src_span_w,
                                      int64_t src_span_h,
                                      int64_t dst_span_w,
                                      int64_t dst_span_h,
                                      int64_t dst_x,
                                      int64_t dst_y,
                                      int64_t wrap) {
    if (!src || !src->data || src->width <= 0 || src->height <= 0 || src_span_w <= 0 ||
        src_span_h <= 0 || dst_span_w <= 0 || dst_span_h <= 0)
        return 0;
    int64_t src_width = src->width;
    int64_t src_height = src->height;
    long double ux =
        (((long double)dst_x + 0.5L) * (long double)src_span_w) / (long double)dst_span_w;
    long double uy =
        (((long double)dst_y + 0.5L) * (long double)src_span_h) / (long double)dst_span_h;
    int64_t off_x =
        ux <= 0.0L ? 0 : (ux >= (long double)src_span_w ? src_span_w - 1 : (int64_t)floorl(ux));
    int64_t off_y =
        uy <= 0.0L ? 0 : (uy >= (long double)src_span_h ? src_span_h - 1 : (int64_t)floorl(uy));
    int64_t sample_x = sampler_region_coord_i64(sx, off_x, src_span_w, src_width, wrap);
    int64_t sample_y = sampler_region_coord_i64(sy, off_y, src_span_h, src_height, wrap);
    return src->data[sample_y * src_width + sample_x];
}

/// @brief Sample one pixel from @p src using bilinear filtering (4-tap blend).
/// @details Same coordinate space as sample_pixels_nearest, but performs a
///          bilinear blend of the four texels surrounding the back-mapped
///          fractional source position. RGB is interpolated in premultiplied
///          alpha space, then converted back to straight RGBA, so transparent
///          edge texels do not darken partially transparent samples.
/// @param src Borrowed source Pixels image.
/// @param sx Source-region origin X.
/// @param sy Source-region origin Y.
/// @param src_span_w Source-region width.
/// @param src_span_h Source-region height.
/// @param dst_span_w Full destination mapping width.
/// @param dst_span_h Full destination mapping height.
/// @param dst_x X coordinate relative to the destination mapping.
/// @param dst_y Y coordinate relative to the destination mapping.
/// @param wrap Source-coordinate wrap mode.
/// @return The filtered `0xRRGGBBAA` texel, or transparent black for degenerate input.
static uint32_t sample_pixels_linear(const rt_pixels_impl *src,
                                     int64_t sx,
                                     int64_t sy,
                                     int64_t src_span_w,
                                     int64_t src_span_h,
                                     int64_t dst_span_w,
                                     int64_t dst_span_h,
                                     int64_t dst_x,
                                     int64_t dst_y,
                                     int64_t wrap) {
    if (!src || !src->data || src->width <= 0 || src->height <= 0 || src_span_w <= 0 ||
        src_span_h <= 0 || dst_span_w <= 0 || dst_span_h <= 0)
        return 0;
    int64_t src_width = src->width;
    int64_t src_height = src->height;
    long double ux =
        (((long double)dst_x + 0.5L) * (long double)src_span_w) / (long double)dst_span_w - 0.5L;
    long double uy =
        (((long double)dst_y + 0.5L) * (long double)src_span_h) / (long double)dst_span_h - 0.5L;
    int64_t base_x;
    int64_t base_y;
    long double tx;
    long double ty;
    if (ux < 0.0L) {
        base_x = -1;
        tx = ux + 1.0L;
    } else if (ux >= (long double)src_span_w) {
        base_x = src_span_w - 1;
        tx = 0.0L;
    } else {
        base_x = (int64_t)floorl(ux);
        tx = ux - (long double)base_x;
    }
    if (uy < 0.0L) {
        base_y = -1;
        ty = uy + 1.0L;
    } else if (uy >= (long double)src_span_h) {
        base_y = src_span_h - 1;
        ty = 0.0L;
    } else {
        base_y = (int64_t)floorl(uy);
        ty = uy - (long double)base_y;
    }

    int64_t x0 = sampler_region_coord_i64(sx, base_x, src_span_w, src_width, wrap);
    int64_t x1 = sampler_region_coord_i64(
        sx, rt2d_saturating_add_i64(base_x, 1), src_span_w, src_width, wrap);
    int64_t y0 = sampler_region_coord_i64(sy, base_y, src_span_h, src_height, wrap);
    int64_t y1 = sampler_region_coord_i64(
        sy, rt2d_saturating_add_i64(base_y, 1), src_span_h, src_height, wrap);

    uint32_t c00 = src->data[y0 * src_width + x0];
    uint32_t c10 = src->data[y0 * src_width + x1];
    uint32_t c01 = src->data[y1 * src_width + x0];
    uint32_t c11 = src->data[y1 * src_width + x1];

    return bilerp_premul_rgba(c00, c10, c01, c11, tx, ty);
}

/// @brief Dispatch a pixel sample to either the nearest or linear filter.
/// @details Single entry point so the inner blit loops don't have to check
///          the filter mode per pixel — only the dispatch sees the filter
///          argument. Defaults to nearest for unknown filter values.
/// @param src Borrowed source Pixels image.
/// @param sx Source-region origin X.
/// @param sy Source-region origin Y.
/// @param src_span_w Source-region width.
/// @param src_span_h Source-region height.
/// @param dst_span_w Full destination mapping width.
/// @param dst_span_h Full destination mapping height.
/// @param dst_x X coordinate relative to the destination mapping.
/// @param dst_y Y coordinate relative to the destination mapping.
/// @param filter Requested `RT_GRAPHICS2D_FILTER_*` mode.
/// @param wrap Requested `RT_GRAPHICS2D_WRAP_*` mode.
/// @return The nearest- or linear-filtered `0xRRGGBBAA` texel.
static uint32_t sample_pixels(const rt_pixels_impl *src,
                              int64_t sx,
                              int64_t sy,
                              int64_t src_span_w,
                              int64_t src_span_h,
                              int64_t dst_span_w,
                              int64_t dst_span_h,
                              int64_t dst_x,
                              int64_t dst_y,
                              int64_t filter,
                              int64_t wrap) {
    if (filter == RT_GRAPHICS2D_FILTER_LINEAR)
        return sample_pixels_linear(
            src, sx, sy, src_span_w, src_span_h, dst_span_w, dst_span_h, dst_x, dst_y, wrap);
    return sample_pixels_nearest(
        src, sx, sy, src_span_w, src_span_h, dst_span_w, dst_span_h, dst_x, dst_y, wrap);
}

/// @brief Blit a sampled, scaled, optionally tinted region from @p src into @p dst.
/// @details Walks the destination rect (dx, dy, dst_width, dst_height), invokes
///          sample_pixels for each pixel to fetch the (possibly filtered)
///          source color, applies the tint and per-call alpha, and composites
///          via blend_pixel. This is the workhorse for Renderer2D.DrawTexture*
///          and post-process passes.
/// @param dst        Destination Pixels (RGBA8). Required.
/// @param dx         Destination rectangle X coordinate.
/// @param dy         Destination rectangle Y coordinate.
/// @param src        Source Pixels (RGBA8). Required.
/// @param sx         Source-region X coordinate.
/// @param sy         Source-region Y coordinate.
/// @param src_width  Width of the source region (the (sx,sy)..(sx+w,sy+h) span).
/// @param src_height Height of the source region.
/// @param dst_width  Width of the destination rect (scaling).
/// @param dst_height Height of the destination rect (scaling).
/// @param tint       `0x00RRGGBB` tint multiplier, or negative for no tint.
/// @param alpha      Extra opacity multiplier in [0, 255].
/// @param blend_mode RT_GRAPHICS2D_BLEND_* (alpha / additive / replace …).
/// @param filter     RT_GRAPHICS2D_FILTER_NEAREST or LINEAR.
/// @param wrap       RT_GRAPHICS2D_WRAP_* for source-coord out-of-range.
static void blit_pixels_sampled_scaled(void *dst,
                                       int64_t dx,
                                       int64_t dy,
                                       void *src,
                                       int64_t sx,
                                       int64_t sy,
                                       int64_t src_width,
                                       int64_t src_height,
                                       int64_t dst_width,
                                       int64_t dst_height,
                                       int64_t tint,
                                       int64_t alpha,
                                       int64_t blend_mode,
                                       int64_t filter,
                                       int64_t wrap) {
    if (!dst || !src || src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0)
        return;
    int64_t target_width = rt_pixels_width(dst);
    int64_t target_height = rt_pixels_height(dst);
    if (target_width <= 0 || target_height <= 0)
        return;

    int64_t clipped_x = 0;
    int64_t clipped_y = 0;
    int64_t copy_width = dst_width;
    int64_t copy_height = dst_height;
    if (dx < 0) {
        uint64_t skip = distance_to_zero_i64(dx);
        if (skip >= (uint64_t)copy_width)
            return;
        clipped_x = (int64_t)skip;
        copy_width -= clipped_x;
        dx = 0;
    }
    if (dy < 0) {
        uint64_t skip = distance_to_zero_i64(dy);
        if (skip >= (uint64_t)copy_height)
            return;
        clipped_y = (int64_t)skip;
        copy_height -= clipped_y;
        dy = 0;
    }
    if (dx >= target_width || dy >= target_height)
        return;
    if (copy_width > target_width - dx)
        copy_width = target_width - dx;
    if (copy_height > target_height - dy)
        copy_height = target_height - dy;
    if (copy_width <= 0 || copy_height <= 0)
        return;

    wrap = wrap == RT_GRAPHICS2D_WRAP_REPEAT ? RT_GRAPHICS2D_WRAP_REPEAT : RT_GRAPHICS2D_WRAP_CLAMP;
    filter = filter == RT_GRAPHICS2D_FILTER_LINEAR ? RT_GRAPHICS2D_FILTER_LINEAR
                                                   : RT_GRAPHICS2D_FILTER_NEAREST;

    void *snapshot = NULL;
    void *read_src = src;
    if (dst == src) {
        snapshot = rt_pixels_clone(src);
        if (!snapshot)
            return;
        read_src = snapshot;
    }
    rt_pixels_impl *si = rt_pixels_checked_impl_or_null(read_src);
    rt_pixels_impl *di = rt_pixels_checked_impl_or_null(dst);
    if (!si || !si->data || !di || !di->data) {
        rt2d_release_ref_slot(&snapshot);
        return;
    }

    int8_t changed = 0;
    for (int64_t y = 0; y < copy_height; y++) {
        int64_t dst_local_y = clipped_y + y;
        uint32_t *drow = &di->data[(dy + y) * di->width + dx];
        for (int64_t x = 0; x < copy_width; x++) {
            int64_t dst_local_x = clipped_x + x;
            uint32_t source = sample_pixels(si,
                                            sx,
                                            sy,
                                            src_width,
                                            src_height,
                                            dst_width,
                                            dst_height,
                                            dst_local_x,
                                            dst_local_y,
                                            filter,
                                            wrap);
            source = apply_tint_alpha(source, tint, alpha);
            /* OPAQUE means "source replaces destination" (rt_graphics2d.h), so it
             * must write even fully-transparent texels to match the canvas raw-copy
             * flush path — otherwise the same command produces different output on a
             * RenderTarget vs the canvas. Only the blending modes skip transparent
             * source texels (they contribute nothing and skipping is faster). */
            if (blend_mode != RT_GRAPHICS2D_BLEND_OPAQUE && (source & 255u) == 0)
                continue;
            uint32_t result = blend_pixel(drow[x], source, blend_mode);
            if (drow[x] != result) {
                drow[x] = result;
                changed = 1;
            }
        }
    }
    if (changed)
        pixels_touch(di);
    rt2d_release_ref_slot(&snapshot);
}

/// @brief Blit a full source image rotated around a pivot inside the source rect.
/// @details The destination anchor `(dx,dy)` is the unrotated top-left; `pivot_x/y`
///          are source-local coordinates. Rotation is clockwise in screen space.
/// @param dst Destination Pixels image modified in place.
/// @param dx Destination X coordinate of the unrotated source image.
/// @param dy Destination Y coordinate of the unrotated source image.
/// @param src Borrowed full source Pixels image.
/// @param pivot_x Rotation pivot X in source-local pixels.
/// @param pivot_y Rotation pivot Y in source-local pixels.
/// @param angle_deg Finite clockwise angle in degrees.
/// @param tint Multiplicative `0x00RRGGBB` tint, or negative for no tint.
/// @param alpha Additional alpha scale in `0..255`.
/// @param blend_mode Requested `RT_GRAPHICS2D_BLEND_*` operation.
/// @param filter Nearest or bilinear texture sampling.
/// @param wrap Clamp or repeat behavior for sampled source coordinates.
static void blit_pixels_rotated(void *dst,
                                int64_t dx,
                                int64_t dy,
                                void *src,
                                int64_t pivot_x,
                                int64_t pivot_y,
                                double angle_deg,
                                int64_t tint,
                                int64_t alpha,
                                int64_t blend_mode,
                                int64_t filter,
                                int64_t wrap) {
    if (!dst || !src || !rt2d_normalize_angle_degrees(&angle_deg))
        return;
    int64_t sw = rt_pixels_width(src);
    int64_t sh = rt_pixels_height(src);
    int64_t dw = rt_pixels_width(dst);
    int64_t dh = rt_pixels_height(dst);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;

    double radians = angle_deg * (double)RT2D_PI / 180.0;
    double cos_a = cos(radians);
    double sin_a = sin(radians);
    double corners[4][2] = {{-(double)pivot_x, -(double)pivot_y},
                            {(double)sw - (double)pivot_x, -(double)pivot_y},
                            {(double)sw - (double)pivot_x, (double)sh - (double)pivot_y},
                            {-(double)pivot_x, (double)sh - (double)pivot_y}};
    double min_x = 1.0e30;
    double min_y = 1.0e30;
    double max_x = -1.0e30;
    double max_y = -1.0e30;
    for (int i = 0; i < 4; i++) {
        double rx = corners[i][0] * cos_a - corners[i][1] * sin_a;
        double ry = corners[i][0] * sin_a + corners[i][1] * cos_a;
        if (rx < min_x)
            min_x = rx;
        if (rx > max_x)
            max_x = rx;
        if (ry < min_y)
            min_y = ry;
        if (ry > max_y)
            max_y = ry;
    }

    int64_t x0;
    int64_t x1;
    int64_t y0;
    int64_t y1;
    if (!rt2d_floor_i64_checked(min_x, &x0) || !rt2d_ceil_i64_checked(max_x, &x1) ||
        !rt2d_floor_i64_checked(min_y, &y0) || !rt2d_ceil_i64_checked(max_y, &y1))
        return;
    wrap = wrap == RT_GRAPHICS2D_WRAP_REPEAT ? RT_GRAPHICS2D_WRAP_REPEAT : RT_GRAPHICS2D_WRAP_CLAMP;
    filter = filter == RT_GRAPHICS2D_FILTER_LINEAR ? RT_GRAPHICS2D_FILTER_LINEAR
                                                   : RT_GRAPHICS2D_FILTER_NEAREST;

    /* Clamp the rotated bounding box to the destination-visible range BEFORE the
     * loops. out = base + o must satisfy 0 <= out < d, i.e. o in [-base, d-base).
     * Without this a user-controlled pivot (via DrawTextureRotatedAt) makes the
     * box span ~1e12 and the loop bodies merely `continue` for trillions of
     * iterations — an effective hang. All arithmetic is saturating so extreme
     * coordinates can never overflow (UB). */
    int64_t dst_base_x = rt2d_saturating_add_i64(dx, pivot_x);
    int64_t dst_base_y = rt2d_saturating_add_i64(dy, pivot_y);
    int64_t x_lo = saturating_sub_i64(0, dst_base_x);
    int64_t x_hi = saturating_sub_i64((int64_t)dw, dst_base_x);
    int64_t y_lo = saturating_sub_i64(0, dst_base_y);
    int64_t y_hi = saturating_sub_i64((int64_t)dh, dst_base_y);
    if (x0 < x_lo)
        x0 = x_lo;
    if (x1 > x_hi)
        x1 = x_hi;
    if (y0 < y_lo)
        y0 = y_lo;
    if (y1 > y_hi)
        y1 = y_hi;
    if (x0 >= x1 || y0 >= y1)
        return;

    void *snapshot = NULL;
    void *read_src = src;
    if (dst == src) {
        snapshot = rt_pixels_clone(src);
        if (!snapshot)
            return;
        read_src = snapshot;
    }
    rt_pixels_impl *si = rt_pixels_checked_impl_or_null(read_src);
    rt_pixels_impl *di = rt_pixels_checked_impl_or_null(dst);
    if (!si || !si->data || !di || !di->data) {
        rt2d_release_ref_slot(&snapshot);
        return;
    }

    int8_t changed = 0;
    for (int64_t oy = y0; oy < y1; oy++) {
        int64_t out_y = rt2d_saturating_add_i64(dst_base_y, oy);
        if (out_y < 0 || out_y >= dh)
            continue;
        for (int64_t ox = x0; ox < x1; ox++) {
            int64_t out_x = rt2d_saturating_add_i64(dst_base_x, ox);
            if (out_x < 0 || out_x >= dw)
                continue;
            double sx = (double)ox * cos_a + (double)oy * sin_a + (double)pivot_x;
            double sy = -(double)ox * sin_a + (double)oy * cos_a + (double)pivot_y;
            if (wrap != RT_GRAPHICS2D_WRAP_REPEAT &&
                (sx < 0.0 || sy < 0.0 || sx >= (double)sw || sy >= (double)sh))
                continue;
            if (wrap == RT_GRAPHICS2D_WRAP_REPEAT) {
                sx = fmod(sx, (double)sw);
                sy = fmod(sy, (double)sh);
                if (sx < 0.0)
                    sx += (double)sw;
                if (sy < 0.0)
                    sy += (double)sh;
            }
            uint32_t source;
            if (filter == RT_GRAPHICS2D_FILTER_LINEAR) {
                int64_t base_x = (int64_t)floor(sx);
                int64_t base_y = (int64_t)floor(sy);
                long double tx = (long double)sx - (long double)base_x;
                long double ty = (long double)sy - (long double)base_y;
                int64_t ix0 = sampler_coord_i64(base_x, sw, wrap);
                int64_t ix1 = sampler_coord_i64(rt2d_saturating_add_i64(base_x, 1), sw, wrap);
                int64_t iy0 = sampler_coord_i64(base_y, sh, wrap);
                int64_t iy1 = sampler_coord_i64(rt2d_saturating_add_i64(base_y, 1), sh, wrap);
                source = bilerp_premul_rgba(si->data[iy0 * si->width + ix0],
                                            si->data[iy0 * si->width + ix1],
                                            si->data[iy1 * si->width + ix0],
                                            si->data[iy1 * si->width + ix1],
                                            tx,
                                            ty);
            } else {
                int64_t ix = (int64_t)floor(sx);
                int64_t iy = (int64_t)floor(sy);
                ix = sampler_coord_i64(ix, sw, wrap);
                iy = sampler_coord_i64(iy, sh, wrap);
                source = si->data[iy * si->width + ix];
            }
            source = apply_tint_alpha(source, tint, alpha);
            /* OPAQUE means "source replaces destination" (rt_graphics2d.h), so it
             * must write even fully-transparent texels to match the canvas raw-copy
             * flush path — otherwise the same command produces different output on a
             * RenderTarget vs the canvas. Only the blending modes skip transparent
             * source texels (they contribute nothing and skipping is faster). */
            if (blend_mode != RT_GRAPHICS2D_BLEND_OPAQUE && (source & 255u) == 0)
                continue;
            uint32_t *dest = &di->data[out_y * di->width + out_x];
            uint32_t result = blend_pixel(*dest, source, blend_mode);
            if (*dest != result) {
                *dest = result;
                changed = 1;
            }
        }
    }
    if (changed)
        pixels_touch(di);
    rt2d_release_ref_slot(&snapshot);
}

/// @brief Tint + alpha-scale every pixel in a buffer in place (no blend, no clipping).
/// @details Used by `Material2D.Apply` and similar "pre-process this Pixels buffer"
///          entry points where the caller wants the tinted copy as a standalone
///          `Pixels` rather than as the source for a blit. Walks the full width×height
///          grid applying `apply_tint_alpha` to each texel; the source alpha is
///          respected so fully-transparent texels stay transparent.
/// @param pixels Pixels image modified in place.
/// @param tint Multiplicative `0x00RRGGBB` tint, or negative for no tint.
/// @param alpha Additional alpha scale, clamped to `0..255` per texel.
static void apply_tint_alpha_in_place(void *pixels, int64_t tint, int64_t alpha) {
    rt_pixels_impl *impl = rt_pixels_checked_impl_or_null(pixels);
    if (!impl || !impl->data)
        return;
    int64_t count = impl->width * impl->height;
    int8_t changed = 0;
    for (int64_t i = 0; i < count; i++) {
        uint32_t result = apply_tint_alpha(impl->data[i], tint, alpha);
        if (impl->data[i] != result) {
            impl->data[i] = result;
            changed = 1;
        }
    }
    if (changed)
        pixels_touch(impl);
}

/// @brief Extract a rectangular sub-region of @p pixels into a new Pixels buffer.
/// @details Allocates a `width × height` Pixels and copies from `(sx, sy)` in the source.
///   The source coordinates are NOT clipped — callers that may have out-of-bounds regions
///   should call `clip_region_to_source` first. Returns NULL if allocation fails or inputs
///   are invalid.
/// @param pixels Borrowed source Pixels image.
/// @param sx Source rectangle X coordinate.
/// @param sy Source rectangle Y coordinate.
/// @param width Positive output width.
/// @param height Positive output height.
/// @return A new owned Pixels image, or `NULL` for invalid input or allocation failure.
void *rt2d_copy_region_pixels(void *pixels, int64_t sx, int64_t sy, int64_t width, int64_t height) {
    if (!pixels || width <= 0 || height <= 0)
        return NULL;
    void *out = rt_pixels_new(width, height);
    if (!out)
        return NULL;
    rt_pixels_copy(out, 0, 0, pixels, sx, sy, width, height);
    return out;
}

/// @brief Clip a source sub-rectangle to the actual Pixels bounds, adjusting destination offset.
/// @details When the source rectangle extends past the left or top edge (negative sx/sy), the
///   overlap is clipped by advancing the destination offset and shrinking the rectangle.
///   When the rectangle runs past the right or bottom edge of the source, width/height are
///   trimmed. Updates all six in-out parameters and returns 1 if a non-empty intersection
///   remains, 0 if the rectangle is fully out of bounds.
/// @param pixels Borrowed source Pixels image defining the clip bounds.
/// @param dx In-out destination X coordinate adjusted for left clipping.
/// @param dy In-out destination Y coordinate adjusted for top clipping.
/// @param sx In-out source X coordinate.
/// @param sy In-out source Y coordinate.
/// @param w In-out rectangle width.
/// @param h In-out rectangle height.
/// @return `1` when a nonempty source intersection remains, otherwise `0`.
static int8_t clip_region_to_source(
    void *pixels, int64_t *dx, int64_t *dy, int64_t *sx, int64_t *sy, int64_t *w, int64_t *h) {
    if (!pixels || !dx || !dy || !sx || !sy || !w || !h || *w <= 0 || *h <= 0)
        return 0;

    int64_t src_w = rt_pixels_width(pixels);
    int64_t src_h = rt_pixels_height(pixels);
    if (src_w <= 0 || src_h <= 0)
        return 0;

    if (*sx < 0) {
        int64_t skip = *sx == INT64_MIN ? *w : -*sx;
        if (skip >= *w)
            return 0;
        if (*dx > INT64_MAX - skip)
            return 0;
        *dx += skip;
        *sx = 0;
        *w -= skip;
    }
    if (*sy < 0) {
        int64_t skip = *sy == INT64_MIN ? *h : -*sy;
        if (skip >= *h)
            return 0;
        if (*dy > INT64_MAX - skip)
            return 0;
        *dy += skip;
        *sy = 0;
        *h -= skip;
    }

    if (*sx >= src_w || *sy >= src_h)
        return 0;
    int64_t src_remaining_w = src_w - *sx;
    int64_t src_remaining_h = src_h - *sy;
    if (*w > src_remaining_w)
        *w = src_remaining_w;
    if (*h > src_remaining_h)
        *h = src_remaining_h;
    return *w > 0 && *h > 0;
}

/// @brief Clone @p pixels and apply tint+alpha in place; returns NULL if no processing is needed.
/// @details The "no-op" condition (tint < 0 and alpha == 255) is checked first to avoid
///   an unnecessary clone when neither tint nor alpha modification is active. Callers use the
///   NULL return to avoid the clone overhead in the common "unmodified" case — if non-NULL is
///   returned the caller owns the clone and must release it.
/// @param pixels Borrowed source Pixels image.
/// @param tint Multiplicative `0x00RRGGBB` tint, or negative for no tint.
/// @param alpha Additional alpha scale.
/// @return A new owned processed clone, or `NULL` when no processing is needed,
///         input is invalid, or allocation fails.
static void *processed_pixels_or_null(void *pixels, int64_t tint, int64_t alpha) {
    if (!pixels || (tint < 0 && clamp_u8_i64(alpha) == 255))
        return NULL;
    void *out = rt_pixels_clone(pixels);
    if (out)
        apply_tint_alpha_in_place(out, tint, alpha);
    return out;
}
