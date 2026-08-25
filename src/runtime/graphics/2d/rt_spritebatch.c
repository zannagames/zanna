//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_spritebatch.c
// Purpose: Implements a retained-command 2D Canvas renderer. Between Begin and
//   End, the batch records Sprite, whole-Pixels, and Pixels-region commands,
//   optionally orders them by depth, applies batch-wide tint/alpha, renders
//   them in one native traversal, and releases their retained sources.
//
// Key invariants:
//   - Commands are accepted only while active, after rt_spritebatch_begin() and
//     before rt_spritebatch_end(). Begin discards any previously queued work.
//   - Every accepted command retains its Sprite or Pixels source until the
//     batch is cleared, ended, begun again, or finalized.
//   - Depth sorting is ascending and deterministic: equal-depth commands retain
//     submission order even though the underlying qsort is not stable.
//   - Sprite commands snapshot position, requested transform, and depth, but
//     sample the retained Sprite's frame, flips, origin, and visibility at End.
//   - Tint, alpha, and sort settings are sampled at End and persist across
//     begin/end cycles until changed or reset.
//   - End clears commands and retains the backing allocation for reuse. The
//     runtime finalizer releases both commands and the allocation.
//
// Ownership/Lifetime:
//   - Constructors return caller-owned runtime reference-counted objects.
//   - Command sources are retained by the batch. Transform intermediates are
//     owned only during End and are released after each draw.
//
// Links: src/runtime/graphics/2d/rt_spritebatch.h (public API),
//        src/runtime/graphics/2d/rt_sprite.h (single-sprite API),
//        docs/zannalib/game.md (SpriteBatch section)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements retained SpriteBatch command collection and rendering.
 *
 * @details An active batch retains submitted Sprite or Pixels sources, records
 *          their draw parameters, optionally orders commands by depth and
 *          submission sequence, applies persistent batch color modulation,
 *          renders to a Canvas, and releases queued ownership after traversal.
 */

#include "rt_spritebatch.h"
#include "rt_graphics.h"
#include "rt_graphics_internal.h"
#include "rt_heap.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_sprite.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

// Forward declaration from rt_io.c
#include "rt_trap.h"

//=============================================================================
// Internal Types
//=============================================================================

/// @brief Default number of draw commands allocated by a new batch.
#define DEFAULT_CAPACITY 256
/// @brief Multiplicative command-storage growth factor.
#define GROWTH_FACTOR 2
/// @brief Hard upper bound on queued commands in one batch.
#define MAX_BATCH_CAPACITY 1048576LL
/// Private initialization cookie for complete SpriteBatch payloads.
#define SPRITEBATCH_STATE_MAGIC UINT64_C(0x5A414E4E41425443)

/// @brief Release a temporary Pixels object created during batch color transforms.
/// @details The original source Pixels object is owned by the batch command; only
///          clones or tinted variants created inside `apply_batch_color` should be
///          released by this helper.
/// @param original Borrowed source Pixels object.
/// @param candidate Potential temporary object to release.
static void release_batch_color_temp(void *original, void *candidate) {
    if (candidate && candidate != original && rt_obj_release_check0(candidate))
        rt_obj_free(candidate);
}

/// @brief Identifies the source and draw path represented by a queued command.
typedef enum { BATCH_ITEM_SPRITE, BATCH_ITEM_PIXELS, BATCH_ITEM_REGION } batch_item_type;

/// @brief Snapshot of one queued SpriteBatch draw command.
/// @details The retained @c source is interpreted according to @c type. Sprite
///          commands use destination/transform/depth fields, whole-Pixels
///          commands use destination fields, and region commands use all source
///          rectangle and transform fields.
typedef struct {
    batch_item_type type;     ///< Queued command variant.
    void *source;             ///< Retained Sprite or Pixels object.
    int64_t x;                ///< Destination X coordinate.
    int64_t y;                ///< Destination Y coordinate.
    int64_t scale_x;          ///< Horizontal scale percentage.
    int64_t scale_y;          ///< Vertical scale percentage.
    int64_t rotation;         ///< Clockwise rotation in degrees.
    int64_t depth;            ///< Primary depth-sort key.
    int64_t src_x;            ///< Region source X coordinate.
    int64_t src_y;            ///< Region source Y coordinate.
    int64_t src_w;            ///< Region source width.
    int64_t src_h;            ///< Region source height.
    int64_t submission_order; ///< Stable secondary sort key.
} batch_item;

/// @brief Private state of one runtime-managed SpriteBatch.
typedef struct {
    uint64_t state_magic;          ///< SPRITEBATCH_STATE_MAGIC after construction.
    batch_item *items;             ///< Owned reusable command allocation.
    int64_t count;                 ///< Number of initialized queued commands.
    int64_t capacity;              ///< Number of allocated command slots.
    int8_t active;                 ///< Whether submissions are currently accepted.
    int8_t sort_by_depth;          ///< Whether End sorts commands before drawing.
    int64_t tint_color;            ///< Persistent batch tint or the no-tint sentinel.
    int64_t alpha;                 ///< Persistent batch alpha multiplier.
    int64_t next_submission_order; ///< Sequence number assigned to the next command.
} spritebatch_impl;

/// @brief Non-owning dimensions/data view used while applying batch alpha.
typedef struct {
    int64_t width;  ///< Pixel-buffer width.
    int64_t height; ///< Pixel-buffer height.
    uint32_t *data; ///< Borrowed mutable RGBA storage.
} spritebatch_pixels_view;

/// @brief Validate-and-return a SpriteBatch pointer; returns NULL for NULL or wrong class.
/// @details Soft check (no trap) — used by every public SpriteBatch entry
///          so that wrong-class handles silently no-op rather than crashing
///          mid-frame during a draw burst.
/// @param batch_ptr Candidate opaque SpriteBatch handle.
/// @return Validated implementation pointer, or `NULL` for null, undersized, or
///         wrong-class objects.
static spritebatch_impl *spritebatch_checked_or_null(void *batch_ptr) {
    if (!batch_ptr ||
        !rt_obj_is_instance(batch_ptr, RT_SPRITEBATCH_CLASS_ID, sizeof(spritebatch_impl)))
        return NULL;
    spritebatch_impl *batch = (spritebatch_impl *)batch_ptr;
    if (batch->state_magic != SPRITEBATCH_STATE_MAGIC || batch->count < 0 || batch->capacity <= 0 ||
        batch->capacity > MAX_BATCH_CAPACITY || batch->count > batch->capacity || !batch->items ||
        (batch->active != 0 && batch->active != 1) ||
        (batch->sort_by_depth != 0 && batch->sort_by_depth != 1) || batch->alpha < 0 ||
        batch->alpha > 255 || batch->next_submission_order < 0)
        return NULL;
    return batch;
}

/// @brief Validate and return a live Pixels implementation without trapping.
/// @param pixels Candidate opaque Pixels handle.
/// @return Validated Pixels implementation, or null for an invalid handle.
static rt_pixels_impl *spritebatch_pixels_checked(void *pixels) {
    return rt_pixels_checked_impl_or_null(pixels);
}

/// @brief Clamp a scale percentage to a minimum of 1 — prevents division by zero in
///        `spritebatch_saturating_scaled_dim` and ensures the sprite remains visible.
/// @param scale Requested integer percentage.
/// @return @p scale when positive, otherwise `1`.
static int64_t spritebatch_normalize_scale(int64_t scale) {
    return scale < 1 ? 1 : scale;
}

/// @brief Sanitize a caller-supplied initial capacity: substitute the default (256)
///        for zero-or-negative values, and cap at MAX_BATCH_CAPACITY to prevent a
///        single over-sized allocation from exhausting heap memory at construction.
/// @param capacity Requested command capacity.
/// @return A capacity in the inclusive range 1 through `MAX_BATCH_CAPACITY`.
static int64_t spritebatch_initial_capacity(int64_t capacity) {
    if (capacity <= 0)
        return DEFAULT_CAPACITY;
    return capacity > MAX_BATCH_CAPACITY ? MAX_BATCH_CAPACITY : capacity;
}

/// @brief Normalize a batch tint color to a color value or the "no tint" sentinel -1.
/// @details Negative values disable tint. Non-negative values pass through so tagged
///   Color.RGBA and raw RGBA alpha reach rt_pixels_tint().
/// @param color Runtime color representation, or a negative no-tint value.
/// @return `-1` for a negative input, otherwise @p color unchanged.
static int64_t spritebatch_normalize_tint(int64_t color) {
    if (color < 0)
        return -1;
    return color;
}

/// @brief Divide an unsigned conceptual 128-bit product by a uint64 divisor.
/// @details Uses quotient/remainder doubling when the native product would
///          overflow, keeping results exact on every supported compiler.
static int8_t spritebatch_unsigned_mul_div(uint64_t lhs,
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
        uint64_t result = product / divisor;
        if (result > limit)
            return 0;
        *quotient = result;
        *remainder = product % divisor;
        return 1;
    }

    uint64_t result_q = 0;
    uint64_t result_r = 0;
    uint64_t term_q = lhs / divisor;
    uint64_t term_r = lhs % divisor;
    uint64_t capped = limit + 1u;
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

/// @brief Compute a scaled pixel dimension with saturation and a minimum of 1.
/// @details Exact integer quotient/remainder arithmetic avoids the
///          platform-dependent precision of `long double`. Half values round
///          upward, matching the transform APIs' positive-dimension policy.
/// @param value Nonnegative source dimension.
/// @param scale Integer percentage, normalized to at least 1.
/// @return Nearest scaled dimension, clamped to at least 1 and saturated to
///         `INT64_MAX`.
static int64_t spritebatch_saturating_scaled_dim(int64_t value, int64_t scale) {
    if (value <= 0)
        return 1;
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    if (!spritebatch_unsigned_mul_div((uint64_t)value,
                                      (uint64_t)spritebatch_normalize_scale(scale),
                                      100u,
                                      (uint64_t)INT64_MAX,
                                      &quotient,
                                      &remainder))
        return INT64_MAX;
    if (remainder >= 50u) {
        if (quotient == (uint64_t)INT64_MAX)
            return INT64_MAX;
        quotient++;
    }
    if (quotient == 0)
        return 1;
    return (int64_t)quotient;
}

/// @brief Return the destination prefix removed when a scaled source is clipped.
/// @details Computing `scaled(full) - scaled(remaining)` keeps the clipped draw's
///          far edge aligned with the originally requested scaled rectangle,
///          including fractional percentages where independently rounding the
///          skipped prefix would add or lose a pixel.
static int64_t spritebatch_scaled_clipped_prefix(int64_t full, int64_t skipped, int64_t scale) {
    if (full <= 0 || skipped <= 0 || skipped >= full)
        return 0;
    int64_t scaled_full = spritebatch_saturating_scaled_dim(full, scale);
    int64_t scaled_remaining = spritebatch_saturating_scaled_dim(full - skipped, scale);
    return scaled_full > scaled_remaining ? scaled_full - scaled_remaining : 0;
}

/// @brief Normalize an integer rotation before conversion to floating point.
/// @param rotation Clockwise rotation in degrees.
/// @return Equivalent angle in the inclusive range 0 through 359.
static int64_t spritebatch_canonical_rotation(int64_t rotation) {
    rotation %= 360;
    if (rotation < 0)
        rotation += 360;
    return rotation;
}

/// @brief Clip a queued region to its immutable Pixels source dimensions.
/// @details Mirrors the source-bound portion of Canvas region clipping before
///          any allocation or transform. Leading source clipping advances the
///          destination, so transformed and fast-path draws have identical
///          placement and neither can allocate attacker-sized off-image pads.
static int8_t spritebatch_clip_region(rt_pixels_impl *pixels,
                                      int64_t *dx,
                                      int64_t *dy,
                                      int64_t *sx,
                                      int64_t *sy,
                                      int64_t *sw,
                                      int64_t *sh,
                                      int64_t scale_x,
                                      int64_t scale_y) {
    if (!pixels || !pixels->data || pixels->width <= 0 || pixels->height <= 0 || !dx || !dy ||
        !sx || !sy || !sw || !sh || *sw <= 0 || *sh <= 0)
        return 0;

    int64_t skip_x = rt_pixels_negative_skip(*sx, *sw);
    int64_t skip_y = rt_pixels_negative_skip(*sy, *sh);
    int64_t destination_skip_x = spritebatch_scaled_clipped_prefix(*sw, skip_x, scale_x);
    int64_t destination_skip_y = spritebatch_scaled_clipped_prefix(*sh, skip_y, scale_y);
    if (skip_x >= *sw || skip_y >= *sh || *dx > INT64_MAX - destination_skip_x ||
        *dy > INT64_MAX - destination_skip_y)
        return 0;
    if (skip_x > 0) {
        *dx += destination_skip_x;
        *sx = 0;
        *sw -= skip_x;
    }
    if (skip_y > 0) {
        *dy += destination_skip_y;
        *sy = 0;
        *sh -= skip_y;
    }
    if (*sx >= pixels->width || *sy >= pixels->height)
        return 0;
    int64_t remaining_w = pixels->width - *sx;
    int64_t remaining_h = pixels->height - *sy;
    if (*sw > remaining_w)
        *sw = remaining_w;
    if (*sh > remaining_h)
        *sh = remaining_h;
    return *sw > 0 && *sh > 0;
}

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief qsort comparator for batch items: primary key is depth (ascending, painter's
///        order), secondary key is submission_order (ascending, preserves insertion order
///        for items at equal depth so draws are deterministic regardless of sort stability).
/// @param a Pointer to the first `batch_item`.
/// @param b Pointer to the second `batch_item`.
/// @return A negative value when @p a sorts first, positive when @p b sorts
///         first, or zero when both keys match.
static int compare_depth(const void *a, const void *b) {
    const batch_item *ia = (const batch_item *)a;
    const batch_item *ib = (const batch_item *)b;
    if (ia->depth < ib->depth)
        return -1;
    if (ia->depth > ib->depth)
        return 1;
    if (ia->submission_order < ib->submission_order)
        return -1;
    if (ia->submission_order > ib->submission_order)
        return 1;
    return 0;
}

/// @brief Release a GC-managed heap payload; skips non-heap pointers (e.g. stack vars).
/// @details `rt_heap_is_payload` guards against releasing static or stack data that was
///   accidentally stored in a batch item's source slot during development.
/// @param obj Candidate runtime heap payload; null and non-payload pointers are ignored.
static void spritebatch_release_object(void *obj) {
    if (!obj || !rt_heap_is_payload(obj))
        return;
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Retain a GC-managed heap payload; skips non-heap pointers.
/// @details Symmetric with `spritebatch_release_object` — used when `add_item` copies
///   an item's source reference into the batch so the batch owns a counted share.
/// @param obj Candidate runtime heap payload; null and non-payload pointers are ignored.
static void spritebatch_retain_object(void *obj) {
    if (!obj || !rt_heap_is_payload(obj))
        return;
    rt_obj_retain_maybe(obj);
}

/// @brief Release a temporary transform result and restore the slot to the original source.
/// @details When a draw pipeline stage (scale, rotate, tint) produces a new Pixels object
///   the temporary replaces @p *slot. This helper releases only that temporary — it
///   guards against releasing when the slot still holds the original (no transform was
///   applied) to avoid a double-free. After release the slot is reset to @p original so
///   subsequent stages see a consistent starting state.
/// @param slot Address of the current working Pixels pointer.
/// @param original Borrowed canonical source that must not be released.
static void spritebatch_release_temp(void **slot, void *original) {
    if (!slot || !*slot || *slot == original)
        return;
    spritebatch_release_object(*slot);
    *slot = original;
}

/// @brief Replace the temporary in @p *slot with @p replacement, releasing the old
///        temporary if it differs from the canonical @p original.
/// @details Complements `spritebatch_release_temp`: used mid-pipeline when one
///   transformed result is immediately replaced by the next (e.g., scaled result fed
///   into the rotate stage). The old temp is freed only when it's truly a temp (not
///   the original source), avoiding spurious releases on the first stage where the
///   slot might still hold the original.
/// @param slot Address of the current working Pixels pointer.
/// @param replacement New transform result to store.
/// @param original Borrowed canonical source that must not be released.
static void spritebatch_replace_temp(void **slot, void *replacement, void *original) {
    if (!slot || !replacement || replacement == *slot)
        return;
    if (*slot && *slot != original)
        spritebatch_release_object(*slot);
    *slot = replacement;
}

/// @brief Release the source Pixels object held by a batch item and zero the struct.
/// @details Zeroing after release ensures that if the item is ever re-used (e.g., from
///   a cleared-and-refilled batch) it starts in a clean state with no stale pointers.
/// @param item Command whose retained source and metadata should be cleared.
static void spritebatch_release_item(batch_item *item) {
    if (!item)
        return;
    spritebatch_release_object(item->source);
    memset(item, 0, sizeof(*item));
}

/// @brief Release all items in the batch and reset the item count to zero.
/// @details Does NOT free or resize the underlying `items` array — capacity is preserved
///   so that subsequent `begin/draw/end` cycles can reuse the same allocation. Called
///   at the end of every `_end` pass and also by the GC finalizer.
/// @param batch Batch whose queued commands should be released.
static void spritebatch_clear_items(spritebatch_impl *batch) {
    if (!batch)
        return;
    for (int64_t i = 0; i < batch->count; ++i)
        spritebatch_release_item(&batch->items[i]);
    batch->count = 0;
}

/// @brief Grow the item array so it can hold at least `batch->count + needed` entries.
/// @details Uses GROWTH_FACTOR doubling (capped at MAX_BATCH_CAPACITY) to amortize
///   realloc costs over many `add` calls. All intermediate multiplication/addition
///   overflow scenarios are checked explicitly so a large `needed` value cannot wrap
///   to a small allocation. Returns 0 (and traps) on allocation failure; the existing
///   array is left intact so callers can partially recover or drain the current batch.
/// @param batch Batch whose item array may need to grow.
/// @param needed Number of additional entries required.
/// @return 1 if the batch has sufficient capacity, 0 on overflow or allocation failure.
static int8_t ensure_capacity(spritebatch_impl *batch, int64_t needed) {
    if (!batch || !batch->items || needed < 0 || batch->count < 0 || batch->capacity <= 0 ||
        batch->count > batch->capacity || batch->capacity > MAX_BATCH_CAPACITY)
        return 0;
    if (needed > INT64_MAX - batch->count)
        return 0;
    int64_t required = batch->count + needed;
    if (required > MAX_BATCH_CAPACITY) {
        /* The hard item cap (not the allocator) is the limiter — trap with a
         * distinct message rather than silently dropping draw commands, which is
         * near-impossible to diagnose in a particle-heavy frame. */
        rt_trap("SpriteBatch: draw-command limit exceeded (1048576 items)");
        return 0;
    }
    if (required <= batch->capacity)
        return 1;

    if (batch->capacity > INT64_MAX / GROWTH_FACTOR)
        return 0;
    int64_t new_capacity = batch->capacity * GROWTH_FACTOR;
    if (new_capacity < required)
        new_capacity = required;
    if (new_capacity > MAX_BATCH_CAPACITY)
        new_capacity = MAX_BATCH_CAPACITY;
    if (new_capacity > INT64_MAX / (int64_t)sizeof(batch_item))
        return 0;

    batch_item *new_items =
        (batch_item *)realloc(batch->items, (size_t)new_capacity * sizeof(batch_item));
    if (!new_items) {
        rt_trap("SpriteBatch: memory allocation failed");
        return 0;
    }
    batch->items = new_items;
    batch->capacity = new_capacity;
    return 1;
}

/// @brief Append one draw command to the batch, retaining its source Pixels reference.
/// @details The submission_order counter is stamped before the copy so depth-equal items
///   remain in insertion order after qsort. The source is retained here (not at draw
///   time) so the caller can release their own reference immediately after calling
///   `add_item` without risk of premature collection.
/// @param batch Active batch that will own the copied command.
/// @param item Command template whose source is retained on successful append.
static void add_item(spritebatch_impl *batch, batch_item *item) {
    if (!batch || batch->next_submission_order == INT64_MAX || !ensure_capacity(batch, 1))
        return;
    spritebatch_retain_object(item->source);
    item->submission_order = batch->next_submission_order++;
    batch->items[batch->count] = *item;
    batch->count++;
}

/// @brief Apply a batch-level tint and/or alpha to a Pixels object, returning either
///        the original (unmodified) or a transformed copy.
/// @details Tint is applied first via `rt_pixels_tint`, which always creates a new
///   object. Alpha modulation requires in-place mutation; if the tint already produced
///   a copy that copy is mutated directly, otherwise a clone is made so the original
///   frame data is never altered. The caller is responsible for releasing any new
///   object returned (i.e., when the result pointer ≠ the input @p pixels pointer).
/// @param pixels      Source Pixels to transform; returned unchanged if no color needed.
/// @param tint_color  24-bit RGB tint, or -1 for none.
/// @param alpha       Global alpha in [0, 255]; 255 means fully opaque (skip alpha pass).
/// @return Transformed Pixels (new object) or @p pixels unchanged.
static void *apply_batch_color(void *pixels, int64_t tint_color, int64_t alpha) {
    if (!pixels || (tint_color < 0 && alpha >= 255))
        return pixels;

    void *result = pixels;
    if (tint_color >= 0) {
        void *tinted = rt_pixels_tint(result, tint_color);
        if (!tinted)
            return NULL;
        result = tinted;
    }

    if (alpha < 255) {
        if (result == pixels) {
            void *cloned = rt_pixels_clone(result);
            if (!cloned) {
                release_batch_color_temp(pixels, result);
                return NULL;
            }
            result = cloned;
        }

        rt_pixels_impl *impl = rt_pixels_checked_impl_or_null(result);
        if (!impl || !impl->data || impl->width <= 0 || impl->height <= 0) {
            release_batch_color_temp(pixels, result);
            return NULL;
        }
        uint64_t width = (uint64_t)impl->width;
        uint64_t height = (uint64_t)impl->height;
        if (width > UINT64_MAX / height) {
            release_batch_color_temp(pixels, result);
            return NULL;
        }
        uint64_t total = width * height;
        if (total > (uint64_t)SIZE_MAX) {
            release_batch_color_temp(pixels, result);
            return NULL;
        }
        uint32_t alpha_u = alpha <= 0 ? 0u : (uint32_t)alpha;
        size_t pixel_count = (size_t)total;
        int8_t changed = 0;
        for (size_t i = 0; i < pixel_count; ++i) {
            uint32_t rgba = impl->data[i];
            uint32_t a = rgba & 0xFFu;
            a = (a * alpha_u + 127u) / 255u;
            uint32_t replacement = (rgba & 0xFFFFFF00u) | a;
            changed |= replacement != rgba;
            impl->data[i] = replacement;
        }
        if (changed)
            pixels_touch(impl);
    }

    return result;
}

/// @brief Crop a rectangular region from @p pixels into a new Pixels object.
/// @details Allocates a fresh Pixels of size sw×sh and copies the source rectangle
///   into it via `rt_pixels_copy`. Used by `draw_region_item` to isolate the source
///   region before applying scale or rotation transforms, which operate on the full
///   image and cannot be constrained to a sub-rectangle directly.
/// @param pixels Borrowed source Pixels object.
/// @param sx Source rectangle's X coordinate.
/// @param sy Source rectangle's Y coordinate.
/// @param sw Positive requested source width.
/// @param sh Positive requested source height.
/// @return New Pixels object containing the extracted region, or NULL on failure.
static void *extract_region_pixels(void *pixels, int64_t sx, int64_t sy, int64_t sw, int64_t sh) {
    if (!pixels || sw <= 0 || sh <= 0)
        return NULL;

    void *region = rt_pixels_new(sw, sh);
    if (!region)
        return NULL;

    rt_pixels_copy(region, 0, 0, pixels, sx, sy, sw, sh);
    return region;
}

/// @brief Draw one batch item to the canvas, applying scale, rotation, and color transforms.
/// @details Fast path: if no transforms or color effects are needed, blits the region
///   with `rt_canvas_blit_region_alpha` (straight-alpha) so transparent sprite-sheet
///   frames composite correctly — the same blending every other batch path uses.
///   Otherwise:
///   1. Extracts the source region into a fresh Pixels object.
///   2. Scales it if scale_x/scale_y ≠ 100%.
///   3. Rotates it if rotation ≠ 0.
///   4. Applies batch-level tint and alpha via `apply_batch_color`.
///   5. Blits the result with `rt_canvas_blit_alpha`, re-centering after rotation so
///      the sprite spins about its own centre instead of drifting (rt_pixels_rotate
///      enlarges the canvas around the centre).
///   Each stage uses `spritebatch_replace_temp` / `spritebatch_release_temp` to
///   ensure the previous intermediate is freed and the source Pixels is never mutated.
/// @param batch Batch supplying the tint and alpha sampled at End.
/// @param canvas Destination Canvas.
/// @param item Region command containing a retained Pixels source.
static void draw_region_item(spritebatch_impl *batch, void *canvas, const batch_item *item) {
    if (!batch || !item || !item->source)
        return;

    int64_t scale_x = spritebatch_normalize_scale(item->scale_x);
    int64_t scale_y = spritebatch_normalize_scale(item->scale_y);

    const bool needsTransform = scale_x != 100 || scale_y != 100 || item->rotation != 0;
    const bool needsColor = batch->tint_color >= 0 || batch->alpha < 255;
    if (!needsTransform && !needsColor) {
        rt_canvas_blit_region_alpha(canvas,
                                    item->x,
                                    item->y,
                                    item->source,
                                    item->src_x,
                                    item->src_y,
                                    item->src_w,
                                    item->src_h);
        return;
    }

    if (item->rotation == 0) {
        int64_t output_width = spritebatch_saturating_scaled_dim(item->src_w, scale_x);
        int64_t output_height = spritebatch_saturating_scaled_dim(item->src_h, scale_y);
        void *transformed = rt_pixels_transform_region_nearest(item->source,
                                                               item->src_x,
                                                               item->src_y,
                                                               item->src_w,
                                                               item->src_h,
                                                               output_width,
                                                               output_height,
                                                               0,
                                                               0,
                                                               batch->tint_color,
                                                               batch->alpha);
        if (!transformed)
            return;
        rt_canvas_blit_alpha(canvas, item->x, item->y, transformed);
        spritebatch_release_object(transformed);
        return;
    }

    void *transformed =
        extract_region_pixels(item->source, item->src_x, item->src_y, item->src_w, item->src_h);
    if (!transformed)
        return;

    /* Track the pre-rotation dimensions so the rotated (enlarged) result can be
     * re-centred at the same point the unrotated image would have occupied. */
    int64_t pre_rot_w = item->src_w;
    int64_t pre_rot_h = item->src_h;

    if (scale_x != 100 || scale_y != 100) {
        int64_t new_w = spritebatch_saturating_scaled_dim(item->src_w, scale_x);
        int64_t new_h = spritebatch_saturating_scaled_dim(item->src_h, scale_y);
        void *scaled = rt_pixels_scale(transformed, new_w, new_h);
        if (!scaled) {
            spritebatch_release_temp(&transformed, item->source);
            return;
        }
        spritebatch_replace_temp(&transformed, scaled, item->source);
        pre_rot_w = new_w;
        pre_rot_h = new_h;
    }

    int64_t blit_x = item->x;
    int64_t blit_y = item->y;
    if (item->rotation != 0) {
        void *rotated = rt_pixels_rotate(transformed, (double)item->rotation);
        if (!rotated) {
            spritebatch_release_temp(&transformed, item->source);
            return;
        }
        spritebatch_replace_temp(&transformed, rotated, item->source);
        /* rt_pixels_rotate expands the canvas around the centre; offset the blit so
         * the enlarged image stays centred on the original region's centre. */
        int64_t rot_w = rt_pixels_width(transformed);
        int64_t rot_h = rt_pixels_height(transformed);
        int64_t offset_x = pre_rot_w / 2 - rot_w / 2;
        int64_t offset_y = pre_rot_h / 2 - rot_h / 2;
        blit_x = rtg_add_sat64(item->x, offset_x);
        blit_y = rtg_add_sat64(item->y, offset_y);
    }

    void *colored = apply_batch_color(transformed, batch->tint_color, batch->alpha);
    if (!colored) {
        spritebatch_release_temp(&transformed, item->source);
        return;
    }
    spritebatch_replace_temp(&transformed, colored, item->source);

    rt_canvas_blit_alpha(canvas, blit_x, blit_y, transformed);
    spritebatch_release_temp(&transformed, item->source);
}

//=============================================================================
// SpriteBatch Creation
//=============================================================================

/// @brief GC finalizer for a SpriteBatch — releases all retained item sources and
///        frees the items array.
/// @details `spritebatch_clear_items` releases each item's source reference; the
///   `items` array itself is freed here because it is a plain heap allocation outside
///   the GC pool.
/// @param obj Candidate SpriteBatch object supplied by the runtime finalizer.
static void spritebatch_finalize(void *obj) {
    if (!obj || !rt_obj_is_instance(obj, RT_SPRITEBATCH_CLASS_ID, sizeof(spritebatch_impl)))
        return;
    spritebatch_impl *batch = (spritebatch_impl *)obj;
    if (batch->state_magic != SPRITEBATCH_STATE_MAGIC)
        return;
    int64_t release_count = batch->count;
    if (!batch->items || release_count < 0 || batch->capacity < 0)
        release_count = 0;
    else if (release_count > batch->capacity)
        release_count = batch->capacity;
    if (release_count > MAX_BATCH_CAPACITY)
        release_count = MAX_BATCH_CAPACITY;
    for (int64_t i = 0; i < release_count; ++i)
        spritebatch_release_item(&batch->items[i]);
    batch->count = 0;
    free(batch->items);
    batch->items = NULL;
}

/// @brief Construct a SpriteBatch with initial command-array capacity. `capacity <= 0` falls
/// back to 256. The batch starts inactive (no tint, alpha 255, depth-sort off). Use
/// `_begin` / draw calls / `_end` to submit batched draws to a Canvas in one pass.
/// @param capacity Requested initial command slots. Nonpositive values select
///                 256 and values above 1,048,576 are capped.
/// @return A caller-owned runtime-managed SpriteBatch reference, or `NULL`
///         after an allocation failure.
void *rt_spritebatch_new(int64_t capacity) {
    spritebatch_impl *batch = (spritebatch_impl *)rt_obj_new_i64(RT_SPRITEBATCH_CLASS_ID,
                                                                 (int64_t)sizeof(spritebatch_impl));
    if (!batch)
        return NULL;
    memset(batch, 0, sizeof(spritebatch_impl));
    batch->state_magic = SPRITEBATCH_STATE_MAGIC;

    capacity = spritebatch_initial_capacity(capacity);
    if (capacity > INT64_MAX / (int64_t)sizeof(batch_item)) {
        rt_trap("SpriteBatch: capacity too large");
        if (rt_obj_release_check0(batch))
            rt_obj_free(batch);
        return NULL;
    }

    batch->items = (batch_item *)malloc((size_t)capacity * sizeof(batch_item));
    if (!batch->items) {
        rt_trap("SpriteBatch: memory allocation failed");
        if (rt_obj_release_check0(batch))
            rt_obj_free(batch);
        return NULL;
    }

    batch->count = 0;
    batch->capacity = capacity;
    batch->active = 0;
    batch->sort_by_depth = 0;
    batch->tint_color = -1;
    batch->alpha = 255;
    batch->next_submission_order = 0;

    rt_obj_set_finalizer(batch, spritebatch_finalize);
    return batch;
}

//=============================================================================
// SpriteBatch Operations
//=============================================================================

/// @brief Begin recording a fresh sequence of draw commands.
/// @details Releases any commands already queued, marks the batch active, and
///          resets submission numbering. Render settings and capacity persist.
///          Null or invalid handles are silently ignored.
/// @param batch_ptr Candidate SpriteBatch handle.
void rt_spritebatch_begin(void *batch_ptr) {
    if (!batch_ptr)
        return;

    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return;
    spritebatch_clear_items(batch);
    batch->active = 1;
    batch->next_submission_order = 0;
}

/// @brief Render and clear all queued commands, then leave recording state.
/// @details An inactive batch is unchanged. A null Canvas discards queued
///          commands without rendering. When enabled, stable depth ordering is
///          emulated by sorting on `(depth, submission_order)`. All retained
///          sources are released after traversal and the backing array is kept.
/// @param batch_ptr Candidate SpriteBatch handle.
/// @param canvas Destination Canvas, or `NULL` to discard the active batch.
void rt_spritebatch_end(void *batch_ptr, void *canvas) {
    if (!batch_ptr)
        return;

    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return;
    if (!batch->active)
        return;
    batch->active = 0;
    if (batch->alpha == 0) {
        spritebatch_clear_items(batch);
        return;
    }
    if (!canvas) {
        spritebatch_clear_items(batch);
        return;
    }

    // Sort by depth if enabled
    if (batch->sort_by_depth && batch->count > 1) {
        qsort(batch->items, (size_t)batch->count, sizeof(batch_item), compare_depth);
    }

    // Render all items
    for (int64_t i = 0; i < batch->count; i++) {
        batch_item *item = &batch->items[i];

        switch (item->type) {
            case BATCH_ITEM_SPRITE:
                if (item->source) {
                    rt_sprite_draw_transformed(item->source,
                                               canvas,
                                               item->x,
                                               item->y,
                                               item->scale_x,
                                               item->scale_y,
                                               item->rotation,
                                               batch->tint_color,
                                               batch->alpha);
                }
                break;

            case BATCH_ITEM_PIXELS:
                if (item->source) {
                    void *draw_src =
                        apply_batch_color(item->source, batch->tint_color, batch->alpha);
                    if (draw_src) {
                        rt_canvas_blit_alpha(canvas, item->x, item->y, draw_src);
                        spritebatch_release_temp(&draw_src, item->source);
                    }
                }
                break;

            case BATCH_ITEM_REGION:
                draw_region_item(batch, canvas, item);
                break;
        }
    }

    spritebatch_clear_items(batch);
}

/// @brief Queue a Sprite draw at 100% scale and zero rotation.
/// @param batch_ptr Candidate active SpriteBatch handle.
/// @param sprite Valid Sprite retained until the batch is cleared.
/// @param x Destination X coordinate for the Sprite origin.
/// @param y Destination Y coordinate for the Sprite origin.
void rt_spritebatch_draw(void *batch_ptr, void *sprite, int64_t x, int64_t y) {
    rt_spritebatch_draw_ex(batch_ptr, sprite, x, y, 100, 100, 0);
}

/// @brief Queue a Sprite draw with uniform percentage scale and zero rotation.
/// @param batch_ptr Candidate active SpriteBatch handle.
/// @param sprite Valid Sprite retained until the batch is cleared.
/// @param x Destination X coordinate for the Sprite origin.
/// @param y Destination Y coordinate for the Sprite origin.
/// @param scale Horizontal and vertical percentage scale.
void rt_spritebatch_draw_scaled(
    void *batch_ptr, void *sprite, int64_t x, int64_t y, int64_t scale) {
    rt_spritebatch_draw_ex(batch_ptr, sprite, x, y, scale, scale, 0);
}

/// @brief Append a sprite draw command to the batch with custom scale (×100) and rotation
/// (degrees). Depth defaults to the sprite's own depth so depth-sort keeps Z-order. Silently
/// no-ops if the batch is not currently `_begin`/`_end`-bracketed.
/// @details Position, transform arguments, and current depth are snapshotted.
///          The retained Sprite's frame, flips, origin, and visibility are read
///          later by `rt_spritebatch_end()`.
/// @param batch_ptr Candidate active SpriteBatch handle.
/// @param sprite Valid Sprite retained until the batch is cleared.
/// @param x Destination X coordinate for the Sprite origin.
/// @param y Destination Y coordinate for the Sprite origin.
/// @param scale_x Horizontal percentage scale.
/// @param scale_y Vertical percentage scale.
/// @param rotation Clockwise rotation in degrees.
void rt_spritebatch_draw_ex(void *batch_ptr,
                            void *sprite,
                            int64_t x,
                            int64_t y,
                            int64_t scale_x,
                            int64_t scale_y,
                            int64_t rotation) {
    if (!batch_ptr || !sprite || !rt_obj_is_instance(sprite, RT_SPRITE_CLASS_ID, 0))
        return;

    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return;
    if (!batch->active)
        return;

    batch_item item = {0};
    item.type = BATCH_ITEM_SPRITE;
    item.source = sprite;
    item.x = x;
    item.y = y;
    item.scale_x = scale_x;
    item.scale_y = scale_y;
    item.rotation = rotation;
    item.depth = rt_sprite_get_depth(sprite);

    add_item(batch, &item);
}

/// @brief Queue a whole-Pixels alpha blit with depth zero.
/// @details The Pixels object is retained until the batch is cleared. Batch tint
///          and alpha are sampled when End renders the command.
/// @param batch_ptr Candidate active SpriteBatch handle.
/// @param pixels Valid Pixels source.
/// @param x Destination top-left X coordinate.
/// @param y Destination top-left Y coordinate.
void rt_spritebatch_draw_pixels(void *batch_ptr, void *pixels, int64_t x, int64_t y) {
    if (!batch_ptr || !spritebatch_pixels_checked(pixels))
        return;

    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return;
    if (!batch->active)
        return;

    batch_item item = {0};
    item.type = BATCH_ITEM_PIXELS;
    item.source = pixels;
    item.x = x;
    item.y = y;
    item.scale_x = 100;
    item.scale_y = 100;
    item.rotation = 0;
    item.depth = 0;

    add_item(batch, &item);
}

/// @brief Append a region (sub-rectangle) draw of `pixels` at (dx, dy) with native size and no
/// rotation. Convenience for drawing one frame from a sprite-sheet without computing transforms.
/// @param batch_ptr Candidate active SpriteBatch handle.
/// @param pixels Valid Pixels source retained until the batch is cleared.
/// @param dx Destination top-left X coordinate.
/// @param dy Destination top-left Y coordinate.
/// @param sx Source rectangle's X coordinate.
/// @param sy Source rectangle's Y coordinate.
/// @param sw Source rectangle's width.
/// @param sh Source rectangle's height.
void rt_spritebatch_draw_region(void *batch_ptr,
                                void *pixels,
                                int64_t dx,
                                int64_t dy,
                                int64_t sx,
                                int64_t sy,
                                int64_t sw,
                                int64_t sh) {
    rt_spritebatch_draw_region_ex(batch_ptr, pixels, dx, dy, sx, sy, sw, sh, 100, 100, 0, 0);
}

/// @brief Full region-draw command: source rect within `pixels`, destination (dx, dy), per-axis
/// scale (×100), rotation (degrees), and explicit Z `depth`. The depth-sort pass uses `depth`
/// when enabled — lower values draw first (behind higher).
/// @details Invalid batches/Pixels and inactive batches are silently ignored.
///          Scaling is clamped to at least 1%; rotation preserves the unrotated
///          region center after the transformed allocation expands.
/// @param batch_ptr Candidate active SpriteBatch handle.
/// @param pixels Valid Pixels source retained until the batch is cleared.
/// @param dx Destination top-left X coordinate before rotation recentering.
/// @param dy Destination top-left Y coordinate before rotation recentering.
/// @param sx Source rectangle's X coordinate.
/// @param sy Source rectangle's Y coordinate.
/// @param sw Source rectangle's width.
/// @param sh Source rectangle's height.
/// @param scale_x Horizontal percentage scale.
/// @param scale_y Vertical percentage scale.
/// @param rotation Clockwise rotation in degrees.
/// @param depth Explicit sort key.
void rt_spritebatch_draw_region_ex(void *batch_ptr,
                                   void *pixels,
                                   int64_t dx,
                                   int64_t dy,
                                   int64_t sx,
                                   int64_t sy,
                                   int64_t sw,
                                   int64_t sh,
                                   int64_t scale_x,
                                   int64_t scale_y,
                                   int64_t rotation,
                                   int64_t depth) {
    rt_pixels_impl *pixels_impl = spritebatch_pixels_checked(pixels);
    if (!batch_ptr || !pixels_impl)
        return;

    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return;
    if (!batch->active)
        return;
    scale_x = spritebatch_normalize_scale(scale_x);
    scale_y = spritebatch_normalize_scale(scale_y);
    if (!spritebatch_clip_region(pixels_impl, &dx, &dy, &sx, &sy, &sw, &sh, scale_x, scale_y))
        return;

    batch_item item = {0};
    item.type = BATCH_ITEM_REGION;
    item.source = pixels;
    item.x = dx;
    item.y = dy;
    item.src_x = sx;
    item.src_y = sy;
    item.src_w = sw;
    item.src_h = sh;
    item.scale_x = scale_x;
    item.scale_y = scale_y;
    item.rotation = spritebatch_canonical_rotation(rotation);
    item.depth = depth;

    add_item(batch, &item);
}

//=============================================================================
// SpriteBatch Properties
//=============================================================================

/// @brief Get the number of currently queued commands.
/// @param batch_ptr Candidate SpriteBatch handle.
/// @return The command count, or `0` for an invalid batch.
int64_t rt_spritebatch_count(void *batch_ptr) {
    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return 0;
    return batch->count;
}

/// @brief Get the allocated command capacity.
/// @param batch_ptr Candidate SpriteBatch handle.
/// @return Number of command slots, or `0` for an invalid batch.
int64_t rt_spritebatch_capacity(void *batch_ptr) {
    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return 0;
    return batch->capacity;
}

/// @brief Report whether the batch is recording commands.
/// @param batch_ptr Candidate SpriteBatch handle.
/// @return `1` after Begin and before a successful active End; otherwise `0`.
int8_t rt_spritebatch_is_active(void *batch_ptr) {
    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return 0;
    return batch->active;
}

//=============================================================================
// SpriteBatch Settings
//=============================================================================

/// @brief Enable or disable ascending depth sorting at End.
/// @details Equal-depth commands preserve submission order. The setting persists
///          across begin/end cycles and is sampled when End is called.
/// @param batch_ptr Candidate SpriteBatch handle.
/// @param enabled Zero for submission order; nonzero for depth order.
void rt_spritebatch_set_sort_by_depth(void *batch_ptr, int8_t enabled) {
    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return;
    batch->sort_by_depth = enabled ? 1 : 0;
}

/// @brief Set the tint applied to every command at End.
/// @param batch_ptr Candidate SpriteBatch handle.
/// @param color Tint accepted by `rt_pixels_tint()`, or any negative value to
///              disable tint.
void rt_spritebatch_set_tint(void *batch_ptr, int64_t color) {
    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return;
    batch->tint_color = spritebatch_normalize_tint(color);
}

/// @brief Set the global alpha multiplier applied to every command at End.
/// @param batch_ptr Candidate SpriteBatch handle.
/// @param alpha Requested value, clamped to the inclusive range 0 through 255.
void rt_spritebatch_set_alpha(void *batch_ptr, int64_t alpha) {
    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return;
    if (alpha < 0)
        alpha = 0;
    if (alpha > 255)
        alpha = 255;
    batch->alpha = alpha;
}

/// @brief Restore submission order, no tint, and fully opaque global alpha.
/// @details Queued commands, capacity, active state, and submission numbering
///          are unchanged.
/// @param batch_ptr Candidate SpriteBatch handle.
void rt_spritebatch_reset_settings(void *batch_ptr) {
    spritebatch_impl *batch = spritebatch_checked_or_null(batch_ptr);
    if (!batch)
        return;
    batch->sort_by_depth = 0;
    batch->tint_color = -1;
    batch->alpha = 255;
}
