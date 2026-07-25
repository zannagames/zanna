//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_graphics2d_internal.h
// Purpose: Shared foundation helpers for the 2D graphics classes — saturating
//          integer math, reference-count slot management, allocation-size
//          checks, and pixel-region copy — used by both rt_graphics2d.c and the
//          tilemap classes in rt_graphics2d_tilemap.c.
//
// Key invariants:
//   - Engine-internal; included only by the graphics/2d/ translation units.
//   - Integer helpers saturate / clamp rather than overflow.
//   - rt2d_copy_region_pixels returns a fresh Pixels object owned by the caller.
//
// Ownership/Lifetime:
//   - rt2d_retain_ref / rt2d_release_ref_slot adjust GC refcounts on borrowed handles.
//
// Links: src/runtime/graphics/2d/rt_graphics2d.c (2D class core),
//        src/runtime/graphics/2d/rt_graphics2d_tilemap.c (tilemap classes)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the shared private foundation for 2D runtime modules.
///
/// These helpers centralize overflow-safe arithmetic, opaque-handle
/// validation, allocation bounds, reference-slot replacement, color
/// normalization, and clipped pixel copying.  The declarations are internal
/// implementation contracts, not part of the public runtime ABI.

#pragma once

#include <stddef.h>
#include <stdint.h>

/// Default capacity used when a growable 2D container receives no positive request.
#define RT2D_INITIAL_CAP 16

/// @brief Adds two signed integers with saturation instead of overflow.
/// @param a Left operand.
/// @param b Right operand.
/// @return `a + b`, saturated at `INT64_MIN` or `INT64_MAX`.
int64_t rt2d_saturating_add_i64(int64_t a, int64_t b);
/// @brief Tests an opaque handle's runtime class.
/// @param obj Opaque handle to validate.
/// @param class_id Required runtime class identifier.
/// @return Nonzero when @p obj is non-null and belongs to @p class_id.
int32_t rt2d_has_class(void *obj, int64_t class_id);
/// @brief Tests an opaque handle's class and minimum payload size.
/// @param obj Opaque handle to validate.
/// @param class_id Required runtime class identifier.
/// @param min_size Minimum payload bytes required before casting.
/// @return Nonzero when both requirements are satisfied.
int32_t rt2d_has_class_min(void *obj, int64_t class_id, size_t min_size);
/// @brief Validates a two-dimensional allocation and computes its element count.
/// @param width Positive requested width.
/// @param height Positive requested height.
/// @param elem_size Positive bytes per element.
/// @param out_count Optional output receiving `width * height` on success.
/// @return `1` when dimensions, element count, and byte count meet the 2D
///         runtime limits; otherwise `0`.
int32_t rt2d_checked_count(int64_t width, int64_t height, int64_t elem_size, int64_t *out_count);
/// @brief Normalizes a caller-requested initial container capacity.
/// @param requested Requested entry count.
/// @return @ref RT2D_INITIAL_CAP for non-positive input, `1048576` above the
///         ceiling, or the positive request unchanged.
int64_t rt2d_initial_capacity(int64_t requested);
/// @brief Retains a GC-managed payload handle when applicable.
/// @param obj Borrowed handle whose payload reference count is incremented;
///        null and non-payload handles are ignored.
void rt2d_retain_ref(void *obj);
/// @brief Clears a reference slot and releases its former payload.
/// @param slot In-out slot to clear; the contained object is freed if its
///        released reference was the last one.
void rt2d_release_ref_slot(void **slot);
/// @brief Copies a source rectangle into a new Pixels image.
/// @param pixels Borrowed source Pixels handle.
/// @param sx Source rectangle X coordinate.
/// @param sy Source rectangle Y coordinate.
/// @param width Positive output width.
/// @param height Positive output height.
/// @return A new owned Pixels handle, or `NULL` for invalid input or allocation
///         failure.
void *rt2d_copy_region_pixels(void *pixels, int64_t sx, int64_t sy, int64_t width, int64_t height);
/// @brief Multiplies two signed integers with saturation instead of overflow.
/// @param a Left operand.
/// @param b Right operand.
/// @return `a * b`, saturated at `INT64_MIN` or `INT64_MAX`.
int64_t rt2d_saturating_mul_i64(int64_t a, int64_t b);
/// @brief Tests whether a handle is a supported bitmap-backed font.
/// @param font Opaque font candidate.
/// @return Nonzero for BitmapFont or SpriteFont instances, otherwise zero.
int32_t rt2d_is_bitmap_font_handle(void *font);
/// @brief Normalizes an RGB, RGBA, or tagged Color value for opaque raster drawing.
/// @param color Source packed or tagged color.
/// @return Normalized `0x00RRGGBB`; alpha is discarded.
int64_t rt2d_draw_rgb(int64_t color);
/// @brief Clips, transforms, and composites a source pixel rectangle.
/// @param dst Borrowed destination Pixels image modified in place.
/// @param dx Destination X coordinate.
/// @param dy Destination Y coordinate.
/// @param src Borrowed source Pixels image.
/// @param sx Source rectangle X coordinate.
/// @param sy Source rectangle Y coordinate.
/// @param width Requested copy width.
/// @param height Requested copy height.
/// @param tint Multiplicative `0x00RRGGBB` tint, or negative to disable tinting.
/// @param alpha Additional alpha scale in `0..255`.
/// @param blend_mode Requested `RT_GRAPHICS2D_BLEND_*` operation.
/// @details Source and destination bounds are clipped, overlapping self-copies
///          are snapshotted, and transparent source texels are skipped except
///          in opaque replacement mode.
void rt2d_blit_pixels(void *dst, int64_t dx, int64_t dy, void *src, int64_t sx, int64_t sy, int64_t width,
                 int64_t height, int64_t tint, int64_t alpha, int64_t blend_mode);
