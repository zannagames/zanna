//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_pixels.c
/// @brief Implements allocation, scalar access, filling, copying, cloning, and
///        raw byte conversion for CPU-side Pixels images.
///
/// @details Pixels storage is an embedded row-major array of raw
/// `0xRRGGBBAA` words. This translation unit validates all dimension products
/// before allocation, traps consistently for invalid object handles, advances
/// mutation generations after successful writes, and gives every object a
/// process-wide nonzero cache identity.
///
// File: src/runtime/graphics/2d/rt_pixels.c
// Purpose: Core operations for Zanna.Graphics.Pixels — the CPU-side software
//   image buffer. Contains allocation, pixel access, fill/clear, copy/clone,
//   and byte conversion. Image I/O lives in rt_pixels_io.c, transforms in
//   rt_pixels_transform.c, and drawing primitives in rt_pixels_draw.c.
//
// Key invariants:
//   - Internal pixel format is 32-bit RGBA: R in bits [31:24], G in [23:16],
//     B in [15:8], A in [7:0] — i.e. 0xRRGGBBAA stored as uint32_t in
//     row-major order (row y starts at data + y * width).
//   - Canvas drawing methods (Box, Disc, etc.) take colors as 0x00RRGGBB
//     (alpha implicitly 0xFF). Pixels Set/Get use 0xRRGGBBAA. Helper
//     rgb_to_rgba(color) = (color << 8) | 0xFF converts between the two.
//   - Drawing primitives use integer coordinates. Sub-pixel rendering is not
//     supported; callers should pre-scale to the physical (HiDPI) resolution.
//   - Flood fill uses an iterative scanline algorithm with a malloc'd stack to
//     avoid recursion depth limits on large images.
//
// Ownership/Lifetime:
//   - Pixels objects are GC-managed (rt_obj_new_i64). The pixel data array is
//     embedded in the GC allocation immediately after rt_pixels_impl.
//
// Links: src/runtime/graphics/2d/rt_pixels_internal.h (shared struct definition),
//        src/runtime/graphics/2d/rt_pixels_io.c (BMP/PNG/JPEG I/O),
//        src/runtime/graphics/2d/rt_pixels_transform.c (geometric and effect ops),
//        src/runtime/graphics/2d/rt_pixels_draw.c (drawing primitives),
//        src/runtime/graphics/2d/rt_pixels.h (public API)
//
//===----------------------------------------------------------------------===//

#include "rt_pixels.h"
#include "rt_pixels_internal.h"

#include "rt_bytes.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_string.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/// @brief Process-wide monotonic counter handed out as cache_identity for new Pixels objects.
/// @details Each Pixels allocation receives a non-zero identity value
///          here so downstream caches (e.g. canvas-side texture caches) can
///          tell two same-dimensions Pixels apart without comparing pointers
///          (which can repeat after GC). Atomic relaxed ordering is sufficient
///          because the value is only a unique identity, not a synchronization primitive.
static uint64_t g_next_pixels_cache_identity = 1;

/// @brief Allocate the next non-zero cache identity without signed overflow.
/// @return A process-wide atomically selected nonzero identity. Wraparound may
///         reuse an identity only after the full `uint64_t` sequence.
static uint64_t pixels_next_cache_identity(void) {
    uint64_t id;
    do {
        id = __atomic_fetch_add(&g_next_pixels_cache_identity, UINT64_C(1), __ATOMIC_RELAXED);
    } while (id == 0);
    return id;
}

/// @brief Raise a runtime trap with a Pixels-specific error message.
/// @details Centralized so every guard in this file (NULL handles, wrong
///          class id) reports the same wording. @p op is preferred when
///          provided; @p fallback covers paths where the operation context
///          isn't known at the trap site.
/// @param op Preferred trap message, or `NULL`.
/// @param fallback Fallback message used when @p op is null.
void zanna_pixels_trap_invalid_handle(const char *op, const char *fallback) {
    rt_trap(op ? op : fallback);
}

/// @brief Validate a Pixels layout and compute safe allocation sizes.
/// @details Guards against three overflow scenarios:
///   1. Negative dimensions (early reject).
///   2. `width * height` int64_t overflow — rejected before computing pixel_count.
///   3. `sizeof(rt_pixels_impl) + data_size` size_t overflow — rejected before
///      passing total_size to rt_obj_new_i64.
///   All outputs are only written on success so callers can pass NULL for
///   fields they don't need.
/// @param width           Buffer width in pixels; must be >= 0.
/// @param height          Buffer height in pixels; must be >= 0.
/// @param pixel_count_out Optional output: total pixel count (width * height).
/// @param data_size_out   Optional output: byte size of the pixel array.
/// @param total_size_out  Optional output: total GC allocation size including header.
/// @return 1 if the layout is valid, 0 if any overflow was detected.
static int32_t pixels_checked_layout(int64_t width,
                                     int64_t height,
                                     int64_t *pixel_count_out,
                                     size_t *data_size_out,
                                     size_t *total_size_out) {
    if (width < 0 || height < 0)
        return 0;

    int64_t pixel_count = 0;
    if (width != 0 && height != 0) {
        if (width > INT64_MAX / height)
            return 0;
        pixel_count = width * height;
    }

    if (pixel_count > (INT64_MAX - (int64_t)sizeof(rt_pixels_impl)) / (int64_t)sizeof(uint32_t))
        return 0;

    size_t data_size = (size_t)pixel_count * sizeof(uint32_t);
    size_t total = sizeof(rt_pixels_impl) + data_size;
    if (total < sizeof(rt_pixels_impl) || total > (size_t)INT64_MAX)
        return 0;

    if (pixel_count_out)
        *pixel_count_out = pixel_count;
    if (data_size_out)
        *data_size_out = data_size;
    if (total_size_out)
        *total_size_out = total;
    return 1;
}

/// @brief Validate a pixel grid's dimensions and compute the raw 4-bytes-per-pixel byte count.
/// @details Delegates the overflow check to pixels_checked_layout, then additionally
///   guards against `pixel_count * 4` int64_t overflow.  Used before allocating or
///   validating a Bytes blob that is expected to hold raw RGBA pixel data.
/// @param width          Buffer width in pixels; must be >= 0.
/// @param height         Buffer height in pixels; must be >= 0.
/// @param byte_count_out Optional output: total byte count (pixel_count * 4).
/// @return 1 if the dimensions are valid, 0 on any overflow.
static int32_t pixels_checked_raw_bytes(int64_t width, int64_t height, int64_t *byte_count_out) {
    int64_t pixel_count = 0;
    if (!pixels_checked_layout(width, height, &pixel_count, NULL, NULL))
        return 0;
    if (pixel_count > INT64_MAX / 4)
        return 0;
    if (byte_count_out)
        *byte_count_out = pixel_count * 4;
    return 1;
}

/// @brief Allocate a new Pixels object with embedded pixel data.
/// @details Zero dimensions are supported and produce a valid object with a
///          null data pointer. Negative dimensions return `NULL` without a trap;
///          overflow and runtime allocation failure trap. Nonempty images are
///          initialized to transparent black with generation zero.
/// @param width Nonnegative image width.
/// @param height Nonnegative image height.
/// @return A new Pixels implementation with a unique cache identity, or `NULL`
///         for invalid dimensions or allocation failure.
rt_pixels_impl *pixels_alloc(int64_t width, int64_t height) {
    if (width < 0 || height < 0)
        return NULL;

    int64_t pixel_count = 0;
    size_t data_size = 0;
    size_t total = 0;
    if (!pixels_checked_layout(width, height, &pixel_count, &data_size, &total)) {
        rt_trap("Pixels: dimensions too large");
        return NULL;
    }

    rt_pixels_impl *pixels = (rt_pixels_impl *)rt_obj_new_i64(RT_PIXELS_CLASS_ID, (int64_t)total);
    if (!pixels) {
        rt_trap("Pixels: memory allocation failed");
        return NULL;
    }

    pixels->width = width;
    pixels->height = height;
    pixels->data =
        pixel_count > 0 ? (uint32_t *)((uint8_t *)pixels + sizeof(rt_pixels_impl)) : NULL;
    pixels->generation = 0;
    pixels->cache_identity = pixels_next_cache_identity();
    pixels->alpha_scan_generation = 0;
    pixels->alpha_classification_scan_count = 0;
    pixels->alpha_scan_valid = 0;
    pixels->alpha_scan_classification = RT_PIXELS_ALPHA_OPAQUE;

    // Zero-fill (transparent black)
    if (pixels->data && data_size > 0)
        memset(pixels->data, 0, data_size);

    return pixels;
}

//=============================================================================
// Constructors
//=============================================================================

/// @brief Construct a Pixels buffer of `width × height` (zero-initialized, transparent).
/// Each pixel is a 32-bit RGBA value (0xRRGGBBAA).
/// @param width Nonnegative image width; zero is permitted.
/// @param height Nonnegative image height; zero is permitted.
/// @return A new Pixels handle, or `NULL` for negative/overflowing dimensions
///         or allocation failure.
void *rt_pixels_new(int64_t width, int64_t height) {
    return pixels_alloc(width, height);
}

//=============================================================================
// Property Accessors
//=============================================================================

/// @brief Pixel buffer width. Traps on null.
/// @param pixels Opaque Pixels handle to query.
/// @return The stored nonnegative width, or `0` after invalid input.
int64_t rt_pixels_width(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Width: invalid pixels");
    if (!p)
        return 0;
    return p->width;
}

/// @brief Pixel buffer height. Traps on null.
/// @param pixels Opaque Pixels handle to query.
/// @return The stored nonnegative height, or `0` after invalid input.
int64_t rt_pixels_height(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Height: invalid pixels");
    if (!p)
        return 0;
    return p->height;
}

/// @brief Return the generation counter, which is bumped on every write to the buffer.
/// @details Used by the GPU upload cache to detect when a Pixels buffer's content has
///   changed since it was last uploaded as a texture, avoiding redundant GPU transfers.
///   Returns 0 for a NULL pixels object.
/// @param pixels Opaque Pixels handle to query; invalid handles are soft failures.
/// @return The current mutation generation, or `0` for invalid input.
uint64_t rt_pixels_generation(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl_or_null(pixels);
    if (!p)
        return 0;
    return p->generation;
}

//=============================================================================
// Pixel Access
//=============================================================================

/// @brief Read the pixel at (x, y) as 0xRRGGBBAA. Out-of-bounds returns 0 (transparent).
/// @param pixels Opaque Pixels handle; invalid handles trap.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @return The raw `0xRRGGBBAA` word, or transparent black for out-of-bounds
///         coordinates or after invalid input.
int64_t rt_pixels_get(void *pixels, int64_t x, int64_t y) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Get: invalid pixels");
    if (!p)
        return 0;

    // Bounds check - return 0 for out of bounds
    if (x < 0 || x >= p->width || y < 0 || y >= p->height)
        return 0;

    int64_t idx = y * p->width + x;
    return (int64_t)p->data[idx];
}

/// @brief Explicit raw-RGBA read alias for scripts that need storage-format pixels.
/// @param pixels Opaque Pixels handle; invalid handles trap.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @return The raw `0xRRGGBBAA` word, or transparent black for out-of-bounds
///         coordinates or after invalid input.
int64_t rt_pixels_get_rgba(void *pixels, int64_t x, int64_t y) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.GetRGBA: invalid pixels");
    if (!p)
        return 0;
    if (x < 0 || x >= p->width || y < 0 || y >= p->height)
        return 0;
    return (int64_t)p->data[y * p->width + x];
}

/// @brief Read a pixel as a Zanna.Graphics.Color-compatible value.
/// @param pixels Opaque Pixels handle; invalid handles trap through GetRGBA.
/// @param x Zero-based column.
/// @param y Zero-based row.
/// @return The raw pixel converted to the tagged Color representation;
///         out-of-bounds coordinates convert transparent black.
int64_t rt_pixels_get_color(void *pixels, int64_t x, int64_t y) {
    uint32_t rgba = (uint32_t)rt_pixels_get_rgba(pixels, x, y);
    return rt_pixels_rgba_to_color(rgba);
}

/// @brief Write a raw uint32_t color value to (x, y) in a Pixels buffer.
/// @details Shared implementation for rt_pixels_set, rt_pixels_set_rgba, and
///   rt_pixels_set_color.  Traps on NULL with the caller-supplied @p trap_op message.
///   Out-of-bounds coordinates are silently ignored (no trap).  Bumps the
///   generation counter via pixels_touch so GPU caches know the content changed.
/// @param pixels   Pixels buffer; traps if NULL.
/// @param x        Column coordinate (0 = left); silently ignored if out of bounds.
/// @param y        Row coordinate (0 = top); silently ignored if out of bounds.
/// @param color    Raw 0xRRGGBBAA pixel value to write.
/// @param trap_op  Trap message used when @p pixels is NULL.
static void rt_pixels_set_raw_internal(
    void *pixels, int64_t x, int64_t y, uint32_t color, const char *trap_op) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, trap_op);
    if (!p)
        return;

    // Bounds check - silently ignore out of bounds
    if (x < 0 || x >= p->width || y < 0 || y >= p->height)
        return;

    int64_t idx = y * p->width + x;
    p->data[idx] = color;
    pixels_touch(p);
}

/// @brief Write `color` at (x, y). Out-of-bounds is a silent no-op.
/// @details Accepts raw `0xRRGGBBAA` or tagged Color.RGBA values; raw values pass
///   through unchanged, tagged values are unpacked from their ARGB+tag form.
/// @param pixels Opaque destination Pixels handle; invalid input traps.
/// @param x Zero-based destination column.
/// @param y Zero-based destination row.
/// @param color Raw RGBA word or explicitly tagged Color value.
void rt_pixels_set(void *pixels, int64_t x, int64_t y, int64_t color) {
    rt_pixels_set_raw_internal(
        pixels, x, y, rt_pixels_rgba_or_tagged_color_to_rgba(color), "Pixels.Set: null pixels");
}

/// @brief Write `rgba` at (x, y), accepting raw `0xRRGGBBAA` or tagged Color.RGBA values.
/// @param pixels Opaque destination Pixels handle; invalid input traps.
/// @param x Zero-based destination column.
/// @param y Zero-based destination row.
/// @param rgba Raw RGBA word or explicitly tagged Color value.
void rt_pixels_set_rgba(void *pixels, int64_t x, int64_t y, int64_t rgba) {
    rt_pixels_set_raw_internal(
        pixels, x, y, rt_pixels_rgba_or_tagged_color_to_rgba(rgba), "Pixels.SetRGBA: null pixels");
}

/// @brief Write Canvas RGB or Color.RGBA at (x, y), converting to raw 0xRRGGBBAA.
/// @param pixels Opaque destination Pixels handle; invalid input traps.
/// @param x Zero-based destination column.
/// @param y Zero-based destination row.
/// @param color Canvas-style `0x00RRGGBB` or explicitly tagged Color value.
void rt_pixels_set_color(void *pixels, int64_t x, int64_t y, int64_t color) {
    rt_pixels_set_raw_internal(
        pixels, x, y, rt_pixels_color_to_rgba(color), "Pixels.SetColor: null pixels");
}

/// @brief Internal: borrow the raw uint32_t pixel array (row-major). NULL-safe. Use sparingly —
/// caller must respect width × height bounds and not free the pointer.
/// @param pixels Opaque Pixels handle; invalid handles are soft failures.
/// @return A borrowed pointer to embedded raw storage, or `NULL` for an invalid
///         or empty image. The pointer is valid only while the object remains
///         alive and must never be freed.
const uint32_t *rt_pixels_raw_buffer(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl_or_null(pixels);
    if (!p)
        return NULL;
    return p->data;
}

//=============================================================================
// Fill Operations
//=============================================================================

/// @brief Fill every pixel in the buffer with a raw uint32_t color value.
/// @details Shared implementation for rt_pixels_fill, rt_pixels_fill_rgba, and
///   rt_pixels_fill_color.  Uses memset when color == 0 (transparent black) for
///   maximum performance, and a simple loop otherwise.  Traps on NULL; no-ops on
///   empty buffers.  Bumps the generation counter via pixels_touch.
/// @param pixels   Pixels buffer; traps if NULL.
/// @param color    Raw 0xRRGGBBAA value to broadcast to every pixel.
/// @param trap_op  Trap message used when @p pixels is NULL.
static void rt_pixels_fill_raw_internal(void *pixels, uint32_t color, const char *trap_op) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, trap_op);
    if (!p)
        return;

    size_t size = 0;
    int64_t pixel_count = 0;
    if (!pixels_checked_layout(p->width, p->height, &pixel_count, &size, NULL)) {
        rt_trap("Pixels.Fill: invalid pixels layout");
        return;
    }
    if (pixel_count == 0 || !p->data)
        return;
    if (color == 0) {
        memset(p->data, 0, size);
        pixels_touch(p);
        return;
    }
    /* Non-zero fill: seed one pixel then broadcast by doubling memcpy — the memcpy
     * runs at memory bandwidth instead of a scalar per-pixel store loop, which is a
     * clear win on the common "clear to an opaque background" path. */
    p->data[0] = color;
    size_t filled = 1;
    size_t total = (size_t)pixel_count;
    while (filled < total) {
        size_t chunk = filled;
        if (chunk > total - filled)
            chunk = total - filled;
        memcpy(p->data + filled, p->data, chunk * sizeof(uint32_t));
        filled += chunk;
    }
    pixels_touch(p);
}

/// @brief Fill the entire buffer with `color`. Optimized for color=0.
/// @details Accepts raw `0xRRGGBBAA` or tagged Color.RGBA values; raw values pass
///   through unchanged, tagged values are unpacked from their ARGB+tag form.
/// @param pixels Opaque destination Pixels handle; invalid input traps.
/// @param color Raw RGBA word or explicitly tagged Color value.
void rt_pixels_fill(void *pixels, int64_t color) {
    rt_pixels_fill_raw_internal(
        pixels, rt_pixels_rgba_or_tagged_color_to_rgba(color), "Pixels.Fill: null pixels");
}

/// @brief Fill with `rgba`, accepting raw `0xRRGGBBAA` or tagged Color.RGBA values.
/// @param pixels Opaque destination Pixels handle; invalid input traps.
/// @param rgba Raw RGBA word or explicitly tagged Color value.
void rt_pixels_fill_rgba(void *pixels, int64_t rgba) {
    rt_pixels_fill_raw_internal(
        pixels, rt_pixels_rgba_or_tagged_color_to_rgba(rgba), "Pixels.FillRGBA: null pixels");
}

/// @brief Fill with Canvas RGB or Color.RGBA, converting to raw 0xRRGGBBAA.
/// @param pixels Opaque destination Pixels handle; invalid input traps.
/// @param color Canvas-style `0x00RRGGBB` or explicitly tagged Color value.
void rt_pixels_fill_color(void *pixels, int64_t color) {
    rt_pixels_fill_raw_internal(
        pixels, rt_pixels_color_to_rgba(color), "Pixels.FillColor: null pixels");
}

/// @brief Reset every pixel to 0 (transparent black). Equivalent to `_fill(pixels, 0)`.
/// @details Nonempty buffers advance their generation; empty buffers remain
///          unchanged.
/// @param pixels Opaque destination Pixels handle; invalid input traps.
void rt_pixels_clear(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Clear: invalid pixels");
    if (!p)
        return;

    size_t size = 0;
    if (!pixels_checked_layout(p->width, p->height, NULL, &size, NULL)) {
        rt_trap("Pixels.Clear: invalid pixels layout");
        return;
    }
    if (p->data && size > 0) {
        memset(p->data, 0, size);
        pixels_touch(p);
    }
}

//=============================================================================
// Copy Operations
//=============================================================================

/// @brief Blit the `w × h` source rectangle (`src` at sx, sy) into `dst` at (dx, dy). Auto-clips
/// to both source and dest bounds; out-of-range pixels are skipped silently.
/// @details Overlapping self-copies use `memmove` and select bottom-up row order
///          when the destination begins below the source. A nonempty clipped
///          copy advances the destination generation.
/// @param dst Opaque destination Pixels handle modified in place.
/// @param dx Destination X coordinate before clipping.
/// @param dy Destination Y coordinate before clipping.
/// @param src Opaque source Pixels handle.
/// @param sx Source X coordinate before clipping.
/// @param sy Source Y coordinate before clipping.
/// @param w Requested copy width.
/// @param h Requested copy height.
void rt_pixels_copy(
    void *dst, int64_t dx, int64_t dy, void *src, int64_t sx, int64_t sy, int64_t w, int64_t h) {
    rt_pixels_impl *d = rt_pixels_checked_impl(dst, "Pixels.Copy: invalid destination pixels");
    rt_pixels_impl *s = rt_pixels_checked_impl(src, "Pixels.Copy: invalid source pixels");
    if (!d || !s)
        return;

    if (!rt_pixels_clip_copy_axis(d->width, s->width, &dx, &sx, &w) ||
        !rt_pixels_clip_copy_axis(d->height, s->height, &dy, &sy, &h))
        return;

    int same_buffer = (d == s);
    int overlap = same_buffer && !(dx + w <= sx || sx + w <= dx || dy + h <= sy || sy + h <= dy);
    int copy_backwards = overlap && dy > sy;

    // Copy row by row. memmove is required for overlapping self-copies.
    for (int64_t row_idx = 0; row_idx < h; row_idx++) {
        int64_t row = copy_backwards ? (h - 1 - row_idx) : row_idx;
        int64_t src_idx = (sy + row) * s->width + sx;
        int64_t dst_idx = (dy + row) * d->width + dx;
        if (overlap)
            memmove(&d->data[dst_idx], &s->data[src_idx], (size_t)w * sizeof(uint32_t));
        else
            memcpy(&d->data[dst_idx], &s->data[src_idx], (size_t)w * sizeof(uint32_t));
    }
    pixels_touch(d);
}

/// @brief Return a deep copy of the buffer (independent storage). Useful before applying
/// destructive transforms or sharing a snapshot across threads.
/// @details Dimensions and raw storage are copied, while the clone receives a
///          new cache identity and begins with generation zero.
/// @param pixels Opaque source Pixels handle; invalid input traps.
/// @return A new independent Pixels handle, or `NULL` after invalid input,
///         layout failure, or allocation failure.
void *rt_pixels_clone(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.Clone: invalid pixels");
    if (!p)
        return NULL;

    rt_pixels_impl *clone = pixels_alloc(p->width, p->height);
    if (!clone)
        return NULL;
    if (p->data && clone->data) {
        size_t size = 0;
        if (!pixels_checked_layout(p->width, p->height, NULL, &size, NULL)) {
            rt_trap("Pixels.Clone: invalid pixels layout");
            if (rt_obj_release_check0(clone))
                rt_obj_free(clone);
            return NULL;
        }
        memcpy(clone->data, p->data, size);
    }
    return clone;
}

//=============================================================================
// Byte Conversion
//=============================================================================

/// @brief Serialize the buffer to a fresh Bytes blob (raw 4 bytes/pixel, row-major). Useful for
/// hashing, persistence, or transmission. Inverse: `_from_bytes`.
/// @details Each storage word is emitted in explicit R, G, B, A byte order, so
///          the serialized representation is independent of host endianness.
/// @param pixels Opaque source Pixels handle; invalid input traps.
/// @return A new Bytes handle of exactly `width * height * 4` bytes, or `NULL`
///         after invalid input, overflow, or allocation failure.
void *rt_pixels_to_bytes(void *pixels) {
    rt_pixels_impl *p = rt_pixels_checked_impl(pixels, "Pixels.ToBytes: invalid pixels");
    if (!p)
        return NULL;

    int64_t byte_count = 0;
    if (!pixels_checked_raw_bytes(p->width, p->height, &byte_count)) {
        rt_trap("Pixels.ToBytes: dimensions too large");
        return NULL;
    }
    void *bytes = rt_bytes_new(byte_count);
    if (!bytes)
        return NULL;

    if (byte_count > 0 && p->data) {
        uint8_t *dst = rt_bytes_data(bytes);
        int64_t count = p->width * p->height;
        for (int64_t i = 0; i < count; ++i) {
            uint32_t rgba = p->data[i];
            dst[i * 4 + 0] = (uint8_t)((rgba >> 24) & 0xFFu);
            dst[i * 4 + 1] = (uint8_t)((rgba >> 16) & 0xFFu);
            dst[i * 4 + 2] = (uint8_t)((rgba >> 8) & 0xFFu);
            dst[i * 4 + 3] = (uint8_t)(rgba & 0xFFu);
        }
    }

    return bytes;
}

/// @brief Deserialize a `width × height` pixel buffer from a Bytes blob (raw RGBA payload).
/// Bytes length must be at least `width * height * 4`; surplus bytes are ignored.
/// @details Input bytes are consumed in explicit R, G, B, A order and assembled
///          into host-independent `0xRRGGBBAA` words. Invalid Bytes handles,
///          unsafe layouts, and insufficient payloads trap; negative dimensions
///          return `NULL` without trapping.
/// @param width Nonnegative output image width.
/// @param height Nonnegative output image height.
/// @param bytes Opaque Bytes handle containing the row-major RGBA payload.
/// @return A new Pixels handle, or `NULL` for invalid dimensions/data or
///         allocation failure.
void *rt_pixels_from_bytes(int64_t width, int64_t height, void *bytes) {
    if (!bytes) {
        rt_trap("Pixels.FromBytes: null bytes");
        return NULL;
    }
    if (!rt_bytes_is_bytes(bytes)) {
        rt_trap("Pixels.FromBytes: invalid bytes");
        return NULL;
    }

    if (width < 0 || height < 0)
        return NULL;

    int64_t required_bytes = 0;
    if (!pixels_checked_raw_bytes(width, height, &required_bytes)) {
        rt_trap("Pixels.FromBytes: dimensions too large");
        return NULL;
    }
    int64_t available_bytes = rt_bytes_len(bytes);

    if (available_bytes < required_bytes) {
        rt_trap("Pixels.FromBytes: insufficient bytes");
        return NULL;
    }

    rt_pixels_impl *p = pixels_alloc(width, height);
    if (!p)
        return NULL;

    if (required_bytes > 0 && p->data) {
        const uint8_t *src = rt_bytes_data_const(bytes);
        if (!src) {
            if (rt_obj_release_check0(p))
                rt_obj_free(p);
            rt_trap("Pixels.FromBytes: invalid bytes");
            return NULL;
        }
        int64_t count = width * height;
        for (int64_t i = 0; i < count; ++i) {
            p->data[i] = ((uint32_t)src[i * 4 + 0] << 24) | ((uint32_t)src[i * 4 + 1] << 16) |
                         ((uint32_t)src[i * 4 + 2] << 8) | (uint32_t)src[i * 4 + 3];
        }
        pixels_touch(p);
    }

    return p;
}
